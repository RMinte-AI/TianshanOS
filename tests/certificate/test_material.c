#include "platform.h"
#include <stdatomic.h>
static _Atomic int64_t clock_now = 1790121600; /* 2026-09-23 UTC */
static time_t test_clock(time_t *out) { time_t t = atomic_load(&clock_now); if(out)*out=t; return t; }
static bool fail_alloc;
static void *test_malloc(size_t n) { return fail_alloc ? NULL : malloc(n); }
#include "mbedtls/x509_crt.h"
static bool fail_parse_alloc;
static int test_parse(mbedtls_x509_crt *crt,const unsigned char *pem,size_t len) {
    return fail_parse_alloc ? MBEDTLS_ERR_X509_ALLOC_FAILED : mbedtls_x509_crt_parse(crt,pem,len);
}
#define mbedtls_x509_crt_parse test_parse
#define time test_clock
#define malloc test_malloc
#include "../../components/ts_cert/src/ts_cert.c"
#undef mbedtls_x509_crt_parse
#undef malloc
#undef time

static char *stored[3];
static unsigned writes, commits, events;
static bool fail_set, fail_commit, fail_read, drop_event;
static int slot(const char *key) { return !strcmp(key,"privkey") ? 0 : !strcmp(key,"cert") ? 1 : !strcmp(key,"ca_chain") ? 2 : -1; }
void *heap_caps_malloc(size_t n,unsigned caps) { (void)caps; return test_malloc(n); }
esp_err_t nvs_open(const char *name,int mode,nvs_handle_t *h) { (void)name;(void)mode;*h=1;return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_get_str(nvs_handle_t h,const char *key,char *out,size_t *len) {
    (void)h; int i=slot(key); if(fail_read)return ESP_FAIL;
    if(i<0||!stored[i])return ESP_ERR_NVS_NOT_FOUND;
    size_t n=strlen(stored[i])+1;
    if(!out){*len=n;return ESP_OK;} if(*len<n)return ESP_ERR_INVALID_SIZE;
    memcpy(out,stored[i],n);*len=n;return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t h,const char *key,const char *value) {
    (void)h; ++writes;if(fail_set)return ESP_FAIL;int i=slot(key);assert(i>=0);
    free(stored[i]);stored[i]=strdup(value);return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t h,const char *key) {
    (void)h;int i=slot(key);if(fail_set)return ESP_FAIL;
    if(i<0||!stored[i])return ESP_ERR_NVS_NOT_FOUND;free(stored[i]);stored[i]=NULL;return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { (void)h;++commits;return fail_commit?ESP_FAIL:ESP_OK; }
esp_err_t ts_event_post(const char *base,int id,const void *data,size_t len,unsigned wait) {
    (void)base;(void)id;(void)data;(void)len;(void)wait;++events;return drop_event?ESP_FAIL:ESP_OK;
}
esp_err_t ts_crypto_keypair_generate(int type,ts_keypair_t *key) {
    (void)type;mbedtls_pk_context *pk=malloc(sizeof(*pk));if(!pk)return ESP_ERR_NO_MEM;
    mbedtls_pk_init(pk);int ret=mbedtls_pk_setup(pk,mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if(!ret)ret=mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1,mbedtls_pk_ec(*pk),mbedtls_ctr_drbg_random,&s_ctr_drbg);
    if(ret){mbedtls_pk_free(pk);free(pk);return ESP_FAIL;}*key=pk;return ESP_OK;
}
esp_err_t ts_crypto_keypair_export_private(ts_keypair_t key,char *out,size_t *len) {
    int ret=mbedtls_pk_write_key_pem(key,(unsigned char *)out,*len);if(ret)return ESP_FAIL;*len=strlen(out)+1;return ESP_OK;
}
void ts_crypto_keypair_free(ts_keypair_t key) {mbedtls_pk_free(key);free(key);}
static char *read_pem(const char *dir,const char *name) {
    char path[1024];snprintf(path,sizeof(path),"%s/%s.pem",dir,name);FILE *f=fopen(path,"rb");assert(f);
    fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);char *p=malloc(n+1);assert(fread(p,1,n,f)==(size_t)n);p[n]=0;fclose(f);return p;
}
static ts_cert_pki_status_t status(void) { ts_cert_pki_status_t s;assert(ts_cert_get_status(&s)==ESP_OK);return s; }
static void install(const char *p) { assert(ts_cert_install_certificate(p,strlen(p)+1)==ESP_OK); }
static void reload(void) { ts_cert_deinit();assert(ts_cert_init()==ESP_OK); }
static char *a,*b,*ca;
static void *writer(void *arg) { (void)arg;for(int i=0;i<100;++i)install(i%2?a:b);return NULL; }
static void *reader(void *arg) { (void)arg;for(int i=0;i<150;++i){ts_cert_snapshot_t snap;assert(ts_cert_get_snapshot(true,&snap)==ESP_OK);assert(!strcmp(snap.certificate,a)||!strcmp(snap.certificate,b));ts_cert_free_snapshot(&snap);status();}return NULL; }
int main(int argc,char **argv) {
    assert(argc==2);a=read_pem(argv[1],"a");b=read_pem(argv[1],"b");ca=read_pem(argv[1],"ca");
    char *key=read_pem(argv[1],"key"),*wrong=read_pem(argv[1],"wrong"),*future=read_pem(argv[1],"future"),*expired=read_pem(argv[1],"expired");
    unsigned n;
    ts_cert_op_error_t detail;
    assert(ts_cert_install_certificate_ex(a,strlen(a)+1,&detail)!=0&&detail==TS_CERT_OP_NOT_INITIALIZED);
    assert(ts_cert_init()==0);
    assert(ts_cert_install_certificate_ex(a,strlen(a)+1,&detail)!=0&&detail==TS_CERT_OP_PRIVATE_KEY_MISSING);
    stored[0]=strdup(key);reload();
    char csr[2048];size_t csr_len=sizeof(csr);
    ts_cert_csr_opts_t opts={.device_id="isolated-test"};
    assert(ts_cert_generate_csr(&opts,csr,&csr_len)==0);
    n=writes;for(int i=0;i<100;++i)assert(status().status==TS_CERT_STATUS_CSR_PENDING);assert(writes==n);
    install(a);
    uint32_t generation=status().generation;n=writes;
    install(a);assert(status().generation==generation&&writes==n);
    assert(ts_cert_install_certificate_ex(wrong,strlen(wrong)+1,&detail)!=0&&detail==TS_CERT_OP_KEY_MISMATCH);
    assert(!strcmp(stored[1],a)&&status().generation==generation);
    assert(ts_cert_install_certificate_ex(a,strlen(a),&detail)!=0&&detail==TS_CERT_OP_INVALID_INPUT);
    const char embedded[]={'a',0,'b',0};assert(ts_cert_install_certificate_ex(embedded,sizeof(embedded),&detail)!=0);
    assert(ts_cert_install_certificate_ex("  \r\n",5,&detail)!=0&&detail==TS_CERT_OP_INVALID_INPUT);
    for(size_t len=3999;len<=4096;++len){char *p=malloc(len);memset(p,'x',len);p[len-1]=0;
        assert(ts_cert_install_certificate_ex(p,len,&detail)!=0);
        assert(detail==(len>4000?TS_CERT_OP_INPUT_TOO_LARGE:TS_CERT_OP_PEM_PARSE_FAILED));free(p);}
    fail_parse_alloc=true;assert(ts_cert_install_certificate_ex(b,strlen(b)+1,&detail)==ESP_ERR_NO_MEM&&detail==TS_CERT_OP_NO_MEMORY);fail_parse_alloc=false;
    fail_alloc=true;assert(ts_cert_install_certificate_ex(b,strlen(b)+1,&detail)==ESP_ERR_NO_MEM&&detail==TS_CERT_OP_NO_MEMORY);fail_alloc=false;
    assert(!strcmp(stored[1],a)&&status().generation==generation);
    fail_set=true;n=events;assert(ts_cert_install_certificate_ex(b,strlen(b)+1,&detail)!=0&&detail==TS_CERT_OP_STORAGE_FAILED);fail_set=false;
    assert(events==n&&status().storage_error&&!strcmp(stored[1],a));reload();
    fail_commit=true;n=events;assert(ts_cert_install_certificate_ex(b,strlen(b)+1,&detail)!=0&&detail==TS_CERT_OP_STORAGE_FAILED);fail_commit=false;
    assert(events==n&&status().storage_error);/* Mock deliberately persists set despite failed commit. */
    assert(!strcmp(stored[1],b));reload();assert(!strcmp(s_certificate_pem,b));install(a);
    assert(!ts_cert_prerequisites(&(ts_cert_pki_status_t){0},true));
    assert(ts_cert_install_ca_chain(ca,strlen(ca)+1)==0);assert(ts_cert_prerequisites(&(ts_cert_pki_status_t){0},false)==false);
    ts_cert_pki_status_t ready=status();assert(ts_cert_prerequisites(&ready,true));
    size_t cl=strlen(ca);char *chain=malloc(2*cl+1);strcpy(chain,ca);strcat(chain,ca);assert(ts_cert_install_ca_chain(chain,strlen(chain)+1)==0);
    strcat(strcpy(chain,ca),"-----BEGIN CERTIFICATE-----\nBAD\n-----END CERTIFICATE-----\n");
    assert(ts_cert_install_ca_chain_ex(chain,strlen(chain)+1,&detail)!=0&&detail==TS_CERT_OP_PEM_PARSE_FAILED);free(chain);
    /* CRLF and no trailing newline are parsed by the SDK's actual mbedTLS. */
    char *crlf=malloc(strlen(a)*2+1),*q=crlf;for(const char *p=a;*p;++p){if(*p=='\n')*q++='\r';*q++=*p;}*q=0;install(crlf);free(crlf);
    char *no_nl=strdup(a);no_nl[strlen(no_nl)-1]=0;install(no_nl);free(no_nl);install(a);
    n=writes;unsigned c=commits;for(int i=0;i<1000;++i)status();assert(writes==n&&commits==c);
    int64_t from=status().cert_info.not_before,to=status().cert_info.not_after;
    const char *tz[]={"UTC","CST-8","PST8PDT"};for(unsigned i=0;i<3;++i){setenv("TZ",tz[i],1);tzset();assert(status().cert_info.not_before==from&&status().cert_info.not_after==to);}
    clock_now=from-1;assert(status().cert_info.validity==TS_CERT_VALIDITY_NOT_YET_VALID);
    clock_now=from;assert(status().cert_info.is_valid);clock_now=to;assert(status().cert_info.is_valid);
    clock_now=to+1;assert(status().cert_info.validity==TS_CERT_VALIDITY_EXPIRED&&status().cert_info.days_until_expiry==-1);
    clock_now=0;assert(status().status==TS_CERT_STATUS_TIME_UNVERIFIED&&!status().cert_info.is_valid);
    clock_now=-1;assert(!status().time_ready);clock_now=1790121600;
    install(future);assert(status().status==TS_CERT_STATUS_NOT_YET_VALID);install(expired);assert(status().status==TS_CERT_STATUS_EXPIRED);install(a);
    drop_event=true;install(b);assert(status().generation>generation);drop_event=false;
    ts_cert_snapshot_t snap;assert(ts_cert_get_snapshot(true,&snap)==0);install(a);assert(!strcmp(snap.certificate,b));assert(snap.generation!=status().generation);ts_cert_free_snapshot(&snap);
    pthread_t t1,t2;pthread_create(&t1,NULL,writer,NULL);pthread_create(&t2,NULL,reader,NULL);pthread_join(t1,NULL);pthread_join(t2,NULL);
    assert(ts_cert_get_snapshot(true,&snap)==0);generation=status().generation;
    assert(ts_cert_generate_keypair()==0);assert(status().generation!=generation&&!status().has_certificate);assert(snap.certificate!=NULL);ts_cert_free_snapshot(&snap);
    assert(ts_cert_delete_keypair()==0);assert(!status().has_private_key&&!status().has_certificate);
    ts_cert_deinit();free(stored[0]);stored[0]=strdup("broken");assert(ts_cert_init()==0);
    assert(ts_cert_install_certificate_ex(a,strlen(a)+1,&detail)!=0&&detail==TS_CERT_OP_PRIVATE_KEY_INVALID);
    ts_cert_deinit();fail_alloc=true;assert(ts_cert_init()==ESP_ERR_NO_MEM);fail_alloc=false;
    ts_cert_pki_status_t empty;memset(&empty,0xAB,sizeof(empty));assert(ts_cert_get_status(&empty)!=0);assert(!empty.has_certificate&&!empty.cert_info.subject_cn[0]);
    ts_cert_deinit();fail_read=true;assert(ts_cert_init()!=0);fail_read=false;assert(!s_private_key_pem&&!s_certificate_pem);
    ts_cert_deinit();for(int i=0;i<3;++i)free(stored[i]);
    free(a);free(b);free(ca);free(key);free(wrong);free(future);free(expired);
    puts("PASS material: real SDK mbedTLS, mocked NVS/allocation/events, state/time/concurrent snapshots");
    return 0;
}
