/* The build extracts the unmodified production lifecycle section (not a model).
 * TLS sockets and certificate snapshot provider are deterministic substitutes. */
#include "lifecycle.inc"
#include <stdatomic.h>
#include <unistd.h>
static _Atomic int init_error,start_error,stop_error,uri_error;
static _Atomic bool block_start, inside_start;
static _Atomic unsigned starts,stops,registrations,allocations;
static _Atomic uint32_t stored_generation=1;
static bool has_ca=true;
static esp_err_t generic_handler(httpd_req_t *r) { (void)r;return ESP_OK; }
esp_err_t ts_cert_get_snapshot(bool require_ca,ts_cert_snapshot_t *out) {
    memset(out,0,sizeof(*out));if(init_error)return init_error;
    if(require_ca&&!has_ca)return ESP_ERR_INVALID_STATE;
    out->key=strdup("key");out->certificate=strdup("cert");if(has_ca)out->ca=strdup("ca");
    out->generation=stored_generation;snprintf(out->certificate_sha256,65,"certificate-%u",stored_generation);++allocations;return ESP_OK;
}
void ts_cert_free_snapshot(ts_cert_snapshot_t *s) {if(s->key)--allocations;free(s->key);free(s->certificate);free(s->ca);memset(s,0,sizeof(*s));}
int httpd_ssl_start(httpd_handle_t *h,httpd_ssl_config_t *c) {(void)c;++starts;inside_start=true;while(block_start)usleep(1000);inside_start=false;if(start_error)return start_error;*h=(void *)1;return ESP_OK;}
int httpd_ssl_stop(httpd_handle_t h) {assert(h);++stops;return stop_error;}
int httpd_register_uri_handler(httpd_handle_t h,const httpd_uri_t *u) {(void)u;assert(h);++registrations;return uri_error;}
static void candidate(void) {
    assert(ts_https_init(NULL)==0);
    s_endpoints[0]=(ts_https_endpoint_t){.uri="/api/auth/whoami"};s_endpoint_count=1;
}
int main(void) {
    ts_https_runtime_t r;
    has_ca=false;assert(ts_https_init(NULL)==ESP_ERR_INVALID_STATE);assert(!allocations);
    ts_https_config_t config=TS_HTTPS_CONFIG_DEFAULT();config.require_client_cert=false;
    assert(ts_https_init(&config)==0);assert(s_ca_chain==NULL);assert(ts_https_start()==0);ts_https_deinit();assert(!allocations);
    has_ca=true;init_error=ESP_ERR_NO_MEM;assert(ts_https_init(NULL)==ESP_ERR_NO_MEM);init_error=0;
    candidate();start_error=ESP_FAIL;assert(ts_https_start()!=0);assert(!ts_https_is_running());ts_https_deinit();assert(!allocations);start_error=0;
    candidate();uri_error=ESP_FAIL;assert(ts_https_start()!=0);assert(!ts_https_is_running());ts_https_deinit();assert(!allocations);uri_error=0;
    candidate();assert(ts_https_start()==0);unsigned before=registrations;
    for(int i=0;i<100;++i)assert(ts_https_start()==0);assert(registrations==before);
    ts_https_get_runtime(&r);assert(r.running&&r.loaded_generation==1);
    stored_generation=2;ts_https_get_runtime(&r);assert(r.loaded_generation==1); // stored B cannot impersonate A
    stop_error=ESP_FAIL;assert(ts_https_stop()!=0);ts_https_deinit();assert(allocations==1&&s_server&&ts_https_is_running());
    stop_error=0;assert(ts_https_stop()==0);ts_https_deinit();assert(!allocations&&!s_server);
    candidate();assert(ts_https_start()==0);ts_https_get_runtime(&r);assert(r.loaded_generation==2&&!strcmp(r.loaded_certificate_sha256,"certificate-2"));ts_https_deinit();
    // Half-start cleanup failure must retain handle/materials without claiming running.
    candidate();uri_error=ESP_FAIL;stop_error=ESP_FAIL;assert(ts_https_start()!=0);assert(s_server&&allocations==1&&!ts_https_is_running());ts_https_deinit();assert(s_server&&allocations==1);
    stop_error=0;uri_error=0;ts_https_deinit();assert(!allocations&&!s_server);
    puts("PASS actual HTTPS lifecycle: init/start/URI/stop failures, retained ownership, duplicate start, loaded generation, optional CA");
    return 0;
}
