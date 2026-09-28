#define main material_suite
#include "test_material.c"
#undef main
#include "cJSON.h"
#include "ts_https.h"
#define TS_API_ERR_INVALID_ARG 1
#define TS_API_ERR_INTERNAL 7
#define TS_API_ERR_NO_MEM 8
#define TS_LOGI(...) ((void)0)
#define TS_LOGE(...) ((void)0)
typedef struct {int code;const char *error;cJSON *data;} ts_api_result_t;
static void ts_api_result_error(ts_api_result_t *r,int code,const char *e) {r->code=code;r->error=e;}
static void ts_api_result_ok(ts_api_result_t *r,cJSON *d) {r->code=0;r->data=d;}
static ts_https_runtime_t runtime={.port=443,.require_client_cert=true};
void ts_https_get_runtime(ts_https_runtime_t *r) {*r=runtime;}
#include "api.inc"
static ts_api_result_t invoke(bool ca_input,const char *pem) {
    cJSON *params=cJSON_CreateObject();cJSON_AddStringToObject(params,ca_input?"ca_pem":"cert_pem",pem);
    ts_api_result_t r={0};int ret=ca_input?api_cert_install_ca(params,&r):api_cert_install(params,&r);
    assert((ret==0)==(r.code==0));cJSON_Delete(params);return r;
}
int main(int argc,char **argv) {
    assert(argc==2);stored[0]=read_pem(argv[1],"key");a=read_pem(argv[1],"a");b=read_pem(argv[1],"b");ca=read_pem(argv[1],"ca");
    assert(ts_cert_init()==0);ts_api_result_t r=invoke(false,a);assert(r.code==0&&!strcmp(stored[1],a));cJSON_Delete(r.data);
    r=invoke(true,ca);assert(r.code==0);cJSON_Delete(r.data);
    cJSON *bad[]={NULL,cJSON_Parse("{}"),cJSON_Parse("{\"cert_pem\":3}"),cJSON_Parse("{\"cert_pem\":\"\"}")};
    for(unsigned i=0;i<4;++i){r=(ts_api_result_t){0};assert(api_cert_install(bad[i],&r)!=0&&r.error);cJSON_Delete(bad[i]);}
    r=invoke(false,"garbage");assert(r.code!=0&&strstr(r.error,"parse"));
    r=(ts_api_result_t){0};assert(api_cert_status(NULL,&r)==0);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(r.data,"prerequisites_satisfied")));
    assert(!cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(r.data,"https"),"running")));cJSON_Delete(r.data);
    runtime.running=true;runtime.loaded_generation=status().generation;strcpy(runtime.loaded_certificate_sha256,"ACTIVE-A");
    r=invoke(false,b);assert(r.code==0);cJSON_Delete(r.data);
    r=(ts_api_result_t){0};assert(api_cert_status(NULL,&r)==0);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(r.data,"restart_required")));
    assert(!strcmp(cJSON_GetObjectItem(cJSON_GetObjectItem(r.data,"https"),"loaded_certificate_sha256")->valuestring,"ACTIVE-A"));cJSON_Delete(r.data);
    clock_now=0;r=(ts_api_result_t){0};assert(api_cert_status(NULL,&r)==0);
    cJSON *info=cJSON_GetObjectItem(r.data,"cert_info");assert(cJSON_IsNull(cJSON_GetObjectItem(info,"seconds_until_expiry")));assert(!cJSON_IsTrue(cJSON_GetObjectItem(info,"is_valid")));cJSON_Delete(r.data);
    ts_cert_deinit();for(int i=0;i<3;++i)free(stored[i]);free(a);free(b);free(ca);
    puts("PASS actual cert API handlers: JSON validation, PEM installation, business errors, additive status, active/stored separation");
    return 0;
}
