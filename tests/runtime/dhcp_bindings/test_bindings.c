#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <pthread.h>
#include <setjmp.h>
#include <time.h>
#include "cJSON.h"
#include "esp_err.h"
#include "esp_netif.h"
#include "dhcpserver/dhcpserver.h"
#include "ts_dhcp_server.h"
#include "ts_net_manager.h"
#include "ts_dhcps_bindings.h"
#define mem_calloc calloc
#define DHCP_COARSE_TIMER_MSECS 1000
#define CONFIG_LWIP_DHCPS_MAX_STATION_NUM 32
#define ESP_DHCPS 1
#define LWIP_TCPIP_CORE_LOCKING 0
#define LWIP_CORE_LOCK_QUERY_HOLDER 1
#define TS_LOGI(...) ((void)0)
#define TS_LOGW(...) ((void)0)
#define TS_LOGE(...) ((void)0)
#define TS_LOGD(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
static void sys_timeout(unsigned ms,void (*fn)(void *),void *arg) {}
#include "sdk_types.inc"
#include "sdk_core.inc"

#define LWIP_ASSERT(message, condition) assert(condition)
#define ip4_addr1(p) ((ntohl((p)->addr)>>24)&255)
#define ip4_addr2(p) ((ntohl((p)->addr)>>16)&255)
#define ip4_addr3(p) ((ntohl((p)->addr)>>8)&255)
#define ip4_addr4(p) (ntohl((p)->addr)&255)
static void get_ip_info(struct netif *n,ip_info_t *info){memset(info,0,sizeof(*info));}
#define CONFIG_LWIP_DHCPS_ADD_DNS 1
#include "sdk_offer.inc"
#undef CONFIG_LWIP_DHCPS_ADD_DNS
#include "sdk_offer_no_dns.inc"
/* Execute the SDK IPC helpers unchanged, with a synchronous host TCP/IP thread.
 * No project state lock is ever acquired on that thread. */
typedef struct esp_netif_api_msg esp_netif_api_msg_t;
typedef esp_err_t (*esp_netif_api_fn)(esp_netif_api_msg_t *);
struct esp_netif_api_msg {esp_netif_t *esp_netif;void *data;esp_netif_api_fn api_fn;esp_err_t ret;};
typedef void (*tcpip_callback_fn)(void *);
static _Thread_local bool on_tcpip;
static unsigned ipc_calls;
static unsigned api_sync_sem,api_lock_sem;
static void sys_sem_signal(unsigned *sem) {}
static void sys_arch_sem_wait(unsigned *sem,unsigned timeout) {}
static bool sys_thread_tcpip(int query) {return on_tcpip;}
struct dispatch {tcpip_callback_fn fn;void *arg;};
static void *run_tcpip(void *arg) {struct dispatch *d=arg;on_tcpip=true;d->fn(d->arg);return NULL;}
static void tcpip_send_msg_wait_sem(tcpip_callback_fn fn,void *arg,void *sem) {
    ++ipc_calls;struct dispatch d={fn,arg};pthread_t thread;
    assert(!pthread_create(&thread,NULL,run_tcpip,&d));assert(!pthread_join(thread,NULL));
}
#define ts_netif_dhcps_replace_bindings sdk_replace_bindings
#include "bridge_core.inc"
#undef ts_netif_dhcps_replace_bindings

typedef pthread_mutex_t *SemaphoreHandle_t;
typedef void *QueueHandle_t;
typedef void *TaskHandle_t;
typedef unsigned TickType_t;
typedef int BaseType_t;
#define portMAX_DELAY -1
#define configTICK_RATE_HZ 1000
#define NVS_READWRITE 1
#define NVS_READONLY 0
#define IPSTR "%u"
#define IP2STR(p) (p)->addr
#define ESP_NETIF_OP_SET 0
#define ESP_NETIF_OP_GET 1
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
#define ESP_NETIF_REQUESTED_IP_ADDRESS 1
#define ESP_NETIF_IP_ADDRESS_LEASE_TIME 2
#define ESP_NETIF_DOMAIN_NAME_SERVER 3
#define ESP_IPADDR_TYPE_V4 0
#define ESP_NETIF_DNS_MAIN 0
#define ESP_NETIF_DNS_BACKUP 1
#define TS_DHCP_IF_ALL TS_DHCP_IF_MAX
static pthread_mutex_t project_mutex=PTHREAD_MUTEX_INITIALIZER;
static unsigned project_lock_depth, manager_lock_depth;
static int xSemaphoreTake(SemaphoreHandle_t mutex,int ticks) {
    assert(!on_tcpip&&!project_lock_depth);assert(!pthread_mutex_lock(mutex));project_lock_depth++;return pdTRUE;
}
static void xSemaphoreGive(SemaphoreHandle_t mutex) {assert(project_lock_depth==1);project_lock_depth--;assert(!pthread_mutex_unlock(mutex));}
#include "project_types.inc"
static ts_dhcp_module_state_t s_state;
static dhcps_t servers[TS_DHCP_IF_MAX];
static esp_netif_ip_info_t infos[TS_DHCP_IF_MAX];
static esp_netif_t netifs[TS_DHCP_IF_MAX];
static bool netifs_exist=true, eth_netif_exists=true, s_link_up;
static bool ts_eth_is_link_up(void) { return s_link_up; }
static bool esp_netif_is_netif_up(esp_netif_t *n) { return n->up; }
static unsigned start_expected_owner;
static bool check_start_table;
static ip4_addr_t ip(unsigned last) {return (ip4_addr_t){htonl(0x0a0a6300U+last)};}
static void drain(dhcps_t *d) {while(d->plist){list_node *n=d->plist;d->plist=n->pnext;free(n->pnode);free(n);}}
static esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key) {
    if(!netifs_exist)return NULL;
    if(!strcmp(key,"WIFI_AP_DEF"))return &netifs[0];
    return !strcmp(key,"ETH_DHCPS")&&eth_netif_exists?&netifs[1]:NULL;
}

/* ESP-IDF 5.5.2 esp_netif_lwip.c:1192-1222,1675-1742,1978,2472.
 * Model esp-netif status separately from the lower-level DHCPS_HANDLE state.
 * No sockets, real event loop or hardware: this is explicitly a host model. */
enum failure_stage {FAIL_NONE, FAIL_STATUS, FAIL_STOP, FAIL_IP, FAIL_POOL, FAIL_LEASE,
                    FAIL_DNS_OFFER, FAIL_DNS_ADDRESS, FAIL_BINDINGS, FAIL_START, FAIL_LEASE_READ};
static enum failure_stage fail_stage;
static unsigned stop_calls, start_calls, apply_dispatches;
static int status_reads_before_failure=-1;
static bool fail_cleanup_stop;
static bool fail_now(enum failure_stage stage) {
    if(fail_stage!=stage)return false;fail_stage=FAIL_NONE;return true;
}
esp_err_t ts_netif_dhcps_replace_bindings(esp_netif_t *n,const ts_dhcps_binding_table_t *table) {
    if(fail_now(FAIL_BINDINGS))return ESP_FAIL;
    return sdk_replace_bindings(n,table);
}
static esp_err_t esp_netif_dhcps_get_status(esp_netif_t *n,esp_netif_dhcp_status_t *status) {
    if(status_reads_before_failure==0||fail_now(FAIL_STATUS))return ESP_FAIL;
    if(status_reads_before_failure>0)status_reads_before_failure--;*status=n->status;return ESP_OK;
}
static esp_err_t esp_netif_dhcps_stop(esp_netif_t *n) {
    stop_calls++;if(fail_cleanup_stop&&stop_calls>1)return ESP_FAIL;
    if(fail_now(FAIL_STOP))return ESP_FAIL;
    if(n->status==ESP_NETIF_DHCP_STOPPED)return ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED;
    if(n->status==ESP_NETIF_DHCP_STARTED){drain(n->dhcps);n->dhcps->state=DHCPS_HANDLE_STOPPED;}
    n->status=ESP_NETIF_DHCP_STOPPED;return ESP_OK;
}
static esp_err_t esp_netif_set_ip_info(esp_netif_t *n,const esp_netif_ip_info_t *i) {
    if(n->status!=ESP_NETIF_DHCP_STOPPED)return ESP_ERR_ESP_NETIF_DHCP_NOT_STOPPED;
    if(fail_now(FAIL_IP))return ESP_FAIL;*n->ip_info=*i;return ESP_OK;
}
static esp_err_t esp_netif_get_ip_info(esp_netif_t *n,esp_netif_ip_info_t *i) {*i=*n->ip_info;return ESP_OK;}
static esp_err_t esp_netif_dhcps_option(esp_netif_t *n,int op,int option,void *value,size_t len) {
    if(op==ESP_NETIF_OP_GET) {
        if(n->status==ESP_NETIF_DHCP_STOPPED)return ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED;
        if(fail_now(FAIL_LEASE_READ))return ESP_FAIL;
        assert(option==ESP_NETIF_IP_ADDRESS_LEASE_TIME);*(uint32_t *)value=n->dhcps->dhcps_lease_time;return ESP_OK;
    }
    if(n->status==ESP_NETIF_DHCP_STARTED)return ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED;
    if(option==ESP_NETIF_REQUESTED_IP_ADDRESS) {
        if(fail_now(FAIL_POOL))return ESP_FAIL;
        dhcps_lease_t *lease=value;uint32_t self=ntohl(n->ip_info->ip.addr);
        uint32_t begin=ntohl(lease->start_ip.addr),end=ntohl(lease->end_ip.addr);
        if(self>=begin&&self<=end)return ESP_ERR_INVALID_ARG;
        n->dhcps->dhcps_poll=*lease;
    }
    if(option==ESP_NETIF_IP_ADDRESS_LEASE_TIME) {
        if(fail_now(FAIL_LEASE))return ESP_FAIL;n->dhcps->dhcps_lease_time=*(uint32_t *)value;
    }
    if(option==ESP_NETIF_DOMAIN_NAME_SERVER) {
        if(fail_now(FAIL_DNS_OFFER))return ESP_FAIL;n->dhcps->dhcps_dns=*(uint8_t *)value;
    }
    return ESP_OK;
}
typedef struct {struct {struct {ip4_addr_t ip4;} u_addr;int type;} ip;} esp_netif_dns_info_t;
typedef int esp_netif_dns_type_t;
typedef struct {esp_netif_dns_type_t dns_type;esp_netif_dns_info_t *dns_info;} esp_netif_dns_param_t;
#define ESP_IP_IS_ANY(ip) ((ip).u_addr.ip4.addr==0)
/* Real SDK public wrapper enforces the nonzero-address contract before IPC. */
static unsigned dns_writes[2];
static esp_err_t esp_netif_set_dns_info_api(esp_netif_api_msg_t *msg) {
    esp_netif_dns_param_t *p=msg->data;
    if(fail_now(FAIL_DNS_ADDRESS))return ESP_FAIL;
    dns_writes[p->dns_type]++;
    msg->esp_netif->dhcps->dns_server[p->dns_type].addr=p->dns_info->ip.u_addr.ip4.addr;return ESP_OK;
}
#include "sdk_dns.inc"
static esp_err_t esp_netif_dhcps_start(esp_netif_t *n) {
    start_calls++;
    if(n->status==ESP_NETIF_DHCP_STARTED)return ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED;
    if(fail_now(FAIL_START)){n->status=ESP_NETIF_DHCP_INIT;return ESP_FAIL;}
    if(!n->up){n->status=ESP_NETIF_DHCP_INIT;return ESP_OK;}
    if(check_start_table) {
        assert(n->dhcps->ts_bindings.count==(start_expected_owner?1:0));
        if(start_expected_owner)assert(n->dhcps->ts_bindings.entries[0].ip==ip(start_expected_owner).addr);
    }
    n->status=ESP_NETIF_DHCP_STARTED;n->dhcps->state=DHCPS_HANDLE_STARTED;
    n->dhcps->server_address=n->ip_info->ip;n->dhcps->dhcps_mask=n->ip_info->netmask;
    n->dhcps->client_address_plus=n->dhcps->dhcps_poll.start_ip;return ESP_OK;
}
struct exec_context {esp_netif_callback_fn fn;void *arg;esp_err_t ret;};
static void *exec_tcpip(void *arg) {
    struct exec_context *ctx=arg;assert(!on_tcpip);on_tcpip=true;
    ctx->ret=ctx->fn(ctx->arg);on_tcpip=false;return NULL;
}
static void (*after_dispatch)(void);
static esp_err_t esp_netif_tcpip_exec(esp_netif_callback_fn fn,void *arg) {
    assert(!on_tcpip);++apply_dispatches;struct exec_context ctx={fn,arg,ESP_FAIL};pthread_t worker;
    assert(!pthread_create(&worker,NULL,exec_tcpip,&ctx));assert(!pthread_join(worker,NULL));
    if(after_dispatch){void (*event)(void)=after_dispatch;after_dispatch=NULL;event();}
    return ctx.ret;
}
static void interface_up(unsigned index) {
    netifs[index].up=true;if(index==1)s_link_up=true;
    if(netifs[index].status==ESP_NETIF_DHCP_INIT)assert(esp_netif_dhcps_start(&netifs[index])==ESP_OK);
}
static TickType_t model_tick=100;
static TickType_t xTaskGetTickCount(void) {return model_tick;}
static unsigned started_notifications, stopped_notifications;
static void notify_event(ts_dhcp_if_t iface,ts_dhcp_event_t e,const ts_dhcp_client_t *client) {
    assert(!on_tcpip&&!project_lock_depth&&!manager_lock_depth);
    if(e==TS_DHCP_EVENT_STARTED)started_notifications++;
    if(e==TS_DHCP_EVENT_STOPPED)stopped_notifications++;
}
const char *ts_dhcp_if_to_str(ts_dhcp_if_t iface) {return iface?"ethernet":"wifi_ap";}
/* NVS key/value model runs the production explicit save and load functions. */
typedef int nvs_handle_t;
static struct {char key[16];unsigned char data[2048];size_t size;int type;int ns;} kv[64];
static size_t kv_count;
static unsigned commits;
static const char *fail_read_key, *fail_write_key;
static bool fail_open_read, missing_namespace, fail_commit;
static esp_err_t nvs_open(const char *name,int mode,nvs_handle_t *handle) {
    assert(!on_tcpip);if(mode==NVS_READONLY&&missing_namespace)return ESP_ERR_NVS_NOT_FOUND;
    if(mode==NVS_READONLY&&fail_open_read)return ESP_FAIL;*handle=!strcmp(name,"ts_net")?2:1;return ESP_OK;
}
static void nvs_close(nvs_handle_t handle) {}
static esp_err_t nvs_commit(nvs_handle_t handle) {assert(!on_tcpip);commits++;return fail_commit?ESP_FAIL:ESP_OK;}
static esp_err_t kv_set_in(int ns,const char *key,const void *data,size_t size) {
    assert(!on_tcpip);if(fail_write_key&&!strcmp(key,fail_write_key))return ESP_FAIL;
    size_t i=0;while(i<kv_count&&(kv[i].ns!=ns||strcmp(kv[i].key,key)))i++;
    if(i==kv_count){assert(kv_count<64);kv[i].ns=ns;strcpy(kv[kv_count++].key,key);}
    assert(size<=sizeof(kv[i].data));memcpy(kv[i].data,data,size);kv[i].size=size;return ESP_OK;
}
static esp_err_t kv_get_in(int ns,const char *key,void *data,size_t *size) {
    assert(!on_tcpip);if(fail_read_key&&!strcmp(key,fail_read_key))return ESP_FAIL;
    size_t i=0;while(i<kv_count&&(kv[i].ns!=ns||strcmp(kv[i].key,key)))i++;
    if(i==kv_count)return ESP_ERR_NVS_NOT_FOUND;
    if(!data){*size=kv[i].size;return ESP_OK;}
    if(*size<kv[i].size){*size=kv[i].size;return ESP_ERR_NVS_INVALID_LENGTH;}
    memcpy(data,kv[i].data,kv[i].size);*size=kv[i].size;return ESP_OK;
}
/* Simulates a stored record whose count and blob length disagree. */
static esp_err_t kv_get(const char *key,void *data,size_t *size) {return kv_get_in(1,key,data,size);}
static bool shrink_blob(const char *key,size_t by) {
    for(size_t i=0;i<kv_count;i++)if(!strcmp(kv[i].key,key)){assert(kv[i].size>by);kv[i].size-=by;return true;}
    return false;
}

enum {KV_U8,KV_U32,KV_STR,KV_BLOB};
static esp_err_t kv_typed_set(int h,const char *key,const void *data,size_t size,int type) {
    int ns=h==2?2:1;esp_err_t ret=kv_set_in(ns,key,data,size);if(ret!=ESP_OK)return ret;
    for(size_t i=0;i<kv_count;i++)if(kv[i].ns==ns&&!strcmp(key,kv[i].key)){kv[i].type=type;break;}return ESP_OK;
}
static esp_err_t kv_typed_get(int h,const char *key,void *data,size_t *size,int type) {
    int ns=h==2?2:1;for(size_t i=0;i<kv_count;i++)if(kv[i].ns==ns&&!strcmp(key,kv[i].key)&&kv[i].type!=type)return ESP_ERR_NVS_TYPE_MISMATCH;
    return kv_get_in(ns,key,data,size);
}
static esp_err_t nvs_set_u8(int h,const char *key,uint8_t value) {return kv_typed_set(h,key,&value,sizeof(value),KV_U8);}
static esp_err_t nvs_set_u32(int h,const char *key,uint32_t value) {return kv_typed_set(h,key,&value,sizeof(value),KV_U32);}
static esp_err_t nvs_set_str(int h,const char *key,const char *value) {return kv_typed_set(h,key,value,strlen(value)+1,KV_STR);}
static esp_err_t nvs_set_blob(int h,const char *key,const void *value,size_t size) {return kv_typed_set(h,key,value,size,KV_BLOB);}
static esp_err_t nvs_get_u8(int h,const char *key,uint8_t *value) {size_t n=sizeof(*value);return kv_typed_get(h,key,value,&n,KV_U8);}
static esp_err_t nvs_get_u32(int h,const char *key,uint32_t *value) {size_t n=sizeof(*value);return kv_typed_get(h,key,value,&n,KV_U32);}
static esp_err_t nvs_get_str(int h,const char *key,char *value,size_t *size) {return kv_typed_get(h,key,value,size,KV_STR);}
static esp_err_t nvs_get_blob(int h,const char *key,void *value,size_t *size) {return kv_typed_get(h,key,value,size,KV_BLOB);}

typedef void *esp_event_base_t;
#define WIFI_EVENT ((esp_event_base_t)1)
#define WIFI_EVENT_AP_STACONNECTED 1
#define WIFI_EVENT_AP_STADISCONNECTED 2
#define WIFI_EVENT_AP_START 3
#define WIFI_EVENT_AP_STOP 4
typedef struct {uint8_t mac[6];unsigned aid;} wifi_event_ap_staconnected_t;
typedef wifi_event_ap_staconnected_t wifi_event_ap_stadisconnected_t;
static dhcp_ip_event_t queued_lease;
static bool lease_ready;
static jmp_buf queue_finished;
static int xQueueReceive(QueueHandle_t queue,dhcp_ip_event_t *event,int ticks) {
    assert(!on_tcpip&&!project_lock_depth);
    if(!lease_ready)longjmp(queue_finished,1);
    *event=queued_lease;lease_ready=false;return pdTRUE;
}
static void esp_ip4addr_ntoa(const ip4_addr_t *ip,char *buffer,size_t len) {
    assert(inet_ntop(AF_INET,&ip->addr,buffer,len));
}
#define IP_EVENT ((esp_event_base_t)2)
#define IP_EVENT_AP_STAIPASSIGNED 1
static SemaphoreHandle_t xSemaphoreCreateMutex(void){return &project_mutex;}
static void vSemaphoreDelete(SemaphoreHandle_t m){}
static QueueHandle_t xQueueCreate(unsigned n,unsigned size){return (void*)1;}
static void vQueueDelete(QueueHandle_t q){}
#define pdPASS 1
static int xTaskCreate(void (*fn)(void*),const char *name,unsigned stack,void *arg,unsigned prio,TaskHandle_t *task){*task=(void*)1;return pdPASS;}
static esp_err_t esp_event_handler_register(esp_event_base_t base,int id,void (*fn)(void*,esp_event_base_t,int32_t,void*),void *arg){return ESP_OK;}
static void ip_event_handler(void *arg,esp_event_base_t base,int32_t id,void *data){}
#include "network_validation.inc"
#include "project_core.inc"
static char console_text[1024];
static const char *esp_err_to_name(esp_err_t err) {return "mock error";}
static void ts_console_printf(const char *fmt,...) {
    size_t n=strlen(console_text);va_list args;va_start(args,fmt);vsnprintf(console_text+n,sizeof(console_text)-n,fmt,args);va_end(args);
}

static const char *iface_display_name(ts_dhcp_if_t iface){return ts_dhcp_if_to_str(iface);}
static void ts_console_success(const char *fmt,...){assert(!on_tcpip);}
static int do_dhcp_clients(ts_dhcp_if_t iface,bool json){return 0;}
static int do_dhcp_start(ts_dhcp_if_t iface){return ts_dhcp_server_start(iface);}
static int do_dhcp_stop(ts_dhcp_if_t iface){return ts_dhcp_server_stop(iface);}
static int do_dhcp_restart(ts_dhcp_if_t iface){return ts_dhcp_server_restart(iface);}
static int do_dhcp_unbind(ts_dhcp_if_t iface,const char *mac){return 0;}
static int do_dhcp_bindings(ts_dhcp_if_t iface,bool json){return 0;}
static int do_dhcp_save(void){return ts_dhcp_server_save_config();}
static int do_dhcp_reset(void){return ts_dhcp_server_reset_config();}
static ts_dhcp_if_t parse_iface(const char *name){return !strcmp(name,"ap")||!strcmp(name,"wifi_ap")?0:!strcmp(name,"eth")||!strcmp(name,"ethernet")?1:TS_DHCP_IF_ALL;}
typedef struct {int code;cJSON *data;const char *message;} ts_api_result_t;
#define TS_API_OK 0
#define TS_API_ERR_INTERNAL 1
#define TS_API_ERR_NO_MEM 2
static void ts_api_result_init(ts_api_result_t *r){memset(r,0,sizeof(*r));}
static void ts_api_result_free(ts_api_result_t *r){cJSON_Delete(r->data);}
static void ts_api_result_error(ts_api_result_t *r,int code,const char *message){r->code=code;r->message=message;}
static void ts_api_result_ok(ts_api_result_t *r,cJSON *data){r->data=data;}
#include "dhcp_status_api.inc"
static esp_err_t ts_api_call(const char *name,const cJSON *params,ts_api_result_t *result){return api_dhcp_status(params,result);}
#include "console_core.inc"


struct arg_lit {int count;};
struct arg_str {int count;const char **sval;};
struct arg_int {int count;int *ival;};
struct arg_end {int count;};
static int arg_parse(int argc,char **argv,void *table){return 0;}
static void arg_print_errors(FILE *f,struct arg_end *end,const char *command){}
#include "console_args.inc"
#include "console_command.inc"
static void cli_defaults(void) {
    static struct arg_lit flags[19];memset(flags,0,sizeof(flags));
    struct arg_lit **slots[]={&s_dhcp_args.status,&s_dhcp_args.list,&s_dhcp_args.clients,&s_dhcp_args.start,
        &s_dhcp_args.stop,&s_dhcp_args.restart,&s_dhcp_args.config,&s_dhcp_args.pool,&s_dhcp_args.bind,
        &s_dhcp_args.bindings,&s_dhcp_args.unbind,&s_dhcp_args.clear_bindings,&s_dhcp_args.save,&s_dhcp_args.reset,
        &s_dhcp_args.json,&s_dhcp_args.help};
    for(unsigned i=0;i<sizeof(slots)/sizeof(*slots);i++)*slots[i]=&flags[i];
    static struct arg_str strings[9];memset(strings,0,sizeof(strings));
    struct arg_str **texts[]={&s_dhcp_args.iface,&s_dhcp_args.start_ip,&s_dhcp_args.end_ip,&s_dhcp_args.gateway,
        &s_dhcp_args.netmask,&s_dhcp_args.dns,&s_dhcp_args.mac,&s_dhcp_args.ip,&s_dhcp_args.hostname};
    for(unsigned i=0;i<sizeof(texts)/sizeof(*texts);i++)*texts[i]=&strings[i];
    static struct arg_int lease;lease.count=0;s_dhcp_args.lease=&lease;
    static struct arg_end end;s_dhcp_args.end=&end;
}
static const uint8_t a[6]={2,0,0,0,0,1},b[6]={2,0,0,0,0,2},c[6]={2,0,0,0,0,3},dmac[6]={2,0,0,0,0,4},e[6]={2,0,0,0,0,5};
static dhcps_t *eth(void) {return &servers[1];}
static void setup(bool erase_nvs) {
    assert(!project_lock_depth);
    for(int i=0;i<TS_DHCP_IF_MAX;i++){drain(&servers[i]);memset(&servers[i],0,sizeof(servers[i]));}
    memset(&s_state,0,sizeof(s_state));s_state.initialized=true;s_state.mutex=&project_mutex;
    for(int i=0;i<TS_DHCP_IF_MAX;i++) {
        set_default_config(&s_state.iface[i].config);
        strcpy(s_state.iface[i].config.pool.gateway,"10.10.99.97");
        /* One-minute host lease: no production clock or defaults are changed. */
        s_state.iface[i].config.lease_time_min=1;
        infos[i]=(esp_netif_ip_info_t){.ip=ip(97),.netmask={htonl(0xffffff00)},.gw=ip(100)};
        netifs[i]=(esp_netif_t){.dhcps=&servers[i],.ip_info=&infos[i],.status=ESP_NETIF_DHCP_STARTED,.up=true};
        s_state.iface[i].applied_config=s_state.iface[i].config;
        s_state.iface[i].applied_valid=true;
        servers[i].dhcps_poll=(dhcps_lease_t){true,ip(100),ip(103)};
        servers[i].client_address_plus=ip(100);servers[i].dhcps_lease_time=1;servers[i].state=DHCPS_HANDLE_STARTED;
    }
    netifs_exist=eth_netif_exists=true;s_link_up=true;initialize_eth_intent();check_start_table=false;console_text[0]=0;
    fail_stage=FAIL_NONE;stop_calls=start_calls=apply_dispatches=0;after_dispatch=NULL;
    memset(dns_writes,0,sizeof(dns_writes));
    model_tick=100;status_reads_before_failure=-1;started_notifications=stopped_notifications=0;
    fail_read_key=fail_write_key=NULL;fail_open_read=missing_namespace=fail_commit=false;fail_cleanup_stop=false;
    if(erase_nvs){memset(kv,0,sizeof(kv));kv_count=commits=0;}
}
static esp_err_t bind_owner_if(ts_dhcp_if_t iface,const uint8_t *mac,unsigned last,bool enabled) {
    ts_dhcp_static_binding_t binding={.enabled=enabled};memcpy(binding.mac,mac,6);
    snprintf(binding.ip,sizeof(binding.ip),"10.10.99.%u",last);
    return ts_dhcp_server_add_static_binding(iface,&binding);
}
static esp_err_t bind_owner(const uint8_t *mac,unsigned last,bool enabled) {
    return bind_owner_if(TS_DHCP_IF_ETH,mac,last,enabled);
}
static unsigned packet(dhcps_t *server,const uint8_t *mac,unsigned type,unsigned current,unsigned request,int expected) {
    dhcps_msg m={.op=1,.htype=1,.hlen=6};memcpy(m.chaddr,mac,6);
    if(current){ip4_addr_t old=ip(current);memcpy(m.ciaddr,&old.addr,4);}
    memcpy(m.options,&magic_cookie,4);unsigned n=4;
    m.options[n++]=DHCP_OPTION_MSG_TYPE;m.options[n++]=1;m.options[n++]=type;
    if(request){ip4_addr_t wanted=ip(request);m.options[n++]=DHCP_OPTION_REQ_IPADDR;m.options[n++]=4;memcpy(m.options+n,&wanted.addr,4);n+=4;}
    m.options[n++]=DHCP_OPTION_END;
    int result=parse_msg(server,&m,n-4);assert(result==expected);
    return ntohl(server->client_address.addr)&255;
}
static unsigned discover(const uint8_t *mac) {return packet(eth(),mac,DHCPDISCOVER,0,0,DHCPS_STATE_OFFER);}
static void request(const uint8_t *mac,unsigned last) {assert(packet(eth(),mac,DHCPREQUEST,0,last,DHCPS_STATE_ACK)==last);}
static void renew(const uint8_t *mac,unsigned last) {assert(packet(eth(),mac,DHCPREQUEST,last,0,DHCPS_STATE_ACK)==last);}
static void release(const uint8_t *mac,unsigned last) {packet(eth(),mac,DHCPRELEASE,last,0,DHCPS_STATE_RELEASE);}
static void advance(unsigned count) {for(unsigned i=0;i<count;i++)dhcps_tmr(eth());}
static struct dhcps_pool *lease(const uint8_t *mac,unsigned last) {
    for(list_node *n=eth()->plist;n;n=n->pnext){struct dhcps_pool *p=n->pnode;if(!memcmp(p->mac,mac,6)&&p->ip.addr==ip(last).addr)return p;}
    return NULL;
}
static void record(const char *name) {printf("{\"case\":\"%s\",\"passed\":true}\n",name);}

static void boot_defaults(bool erase) {
    setup(erase);
    for(int i=0;i<TS_DHCP_IF_MAX;i++) {
        set_interface_defaults(i,&s_state.iface[i].config);s_state.iface[i].applied_valid=false;
        netifs[i].status=ESP_NETIF_DHCP_INIT;netifs[i].up=false;servers[i].state=DHCPS_HANDLE_CREATED;
    }
    s_link_up=false;initialize_eth_intent();
    infos[0]=(esp_netif_ip_info_t){.ip={inet_addr("192.168.4.1")},.netmask={inet_addr("255.255.255.0")},.gw={inet_addr("192.168.4.1")}};
    servers[0].dhcps_lease_time=DHCPS_LEASE_TIME_DEF;servers[0].dhcps_dns=0;
    servers[0].dhcps_poll=(dhcps_lease_t){true,{inet_addr("192.168.4.2")},{inet_addr("192.168.4.101")}};
}
static uint32_t option_value(dhcps_t *server,unsigned option,bool add_dns) {
    unsigned char options[512]={0};unsigned char *end=add_dns?add_offer_options(server,options):add_offer_options_no_dns(server,options);
    for(unsigned char *p=options;p+2<=end;) {
        size_t len=p[1];assert(p+2+len<=end);
        if(p[0]==option){assert(len==4||(option==DHCP_OPTION_DNS_SERVER&&len==8));uint32_t value;memcpy(&value,p+2,4);return value;}
        p+=2+len;
    }
    return 0;
}
static void set_saved_string(ts_dhcp_if_t iface,unsigned field,const char *value) {
    assert(nvs_set_str(0,dhcp_nvs_keys(iface)[field],value)==ESP_OK);
}
static void queued_ap_up(void){interface_up(0);}


static void remove_interface_record(ts_dhcp_if_t iface) {
    const char *const *keys=dhcp_nvs_keys(iface);
    for(unsigned key=0;key<9;key++)for(size_t i=0;i<kv_count;i++)if(!strcmp(kv[i].key,keys[key])) {
        kv[i]=kv[--kv_count];break;
    }
}
static void run_lease_event(const uint8_t *mac,unsigned last) {
    memcpy(queued_lease.mac,mac,6);queued_lease.ip=ip(last);lease_ready=true;
    if(!setjmp(queue_finished))dhcp_event_task(NULL);
    assert(!project_lock_depth);
}
static bool eth_run_requested(void) {
    ts_dhcp_status_t status;assert(ts_dhcp_server_get_status(TS_DHCP_IF_ETH,&status)==ESP_OK);
    return status.want_running;
}
#include "test_eth_lifecycle.inc"

static void check_transitions_and_queries(void) {
    for(ts_dhcp_if_t iface=0;iface<TS_DHCP_IF_MAX;iface++)for(unsigned action=0;action<5;action++) {
        setup(true);s_state.iface[iface].start_tick=42000;model_tick=100000;
        ts_dhcp_config_t pending=s_state.iface[iface].config;
        if(action==2)fail_stage=FAIL_POOL;
        if(action==3){strcpy(pending.pool.start_ip,"10.10.100.100");assert(ts_dhcp_server_set_config(iface,&pending)==ESP_OK);}
        if(action==4)status_reads_before_failure=1; /* Snapshot works, final/cleanup queries fail. */
        esp_err_t ret=action==0?ts_dhcp_server_start(iface):ts_dhcp_server_restart(iface);
        assert((ret==ESP_OK)==(action<2));
        assert(started_notifications==(action==1)&&stopped_notifications==(action==2));
        assert(start_calls==((action==1||action==4)?1:0));
        status_reads_before_failure=-1;
        ts_dhcp_status_t status;assert(ts_dhcp_server_get_status(iface,&status)==ESP_OK);
        assert(status.state==(action<2||action==3?TS_DHCP_STATE_RUNNING:TS_DHCP_STATE_ERROR));
        if(action==0||action==3)assert(stop_calls==0&&status.uptime_sec==58);
        else assert(status.uptime_sec==0);
    }
    record("actual_start_stop_notifications_and_uptime_repeat_restart_failure_prevalidation_unknown");
    setup(true);esp_netif_ip_info_t host=infos[1];fail_stage=FAIL_POOL;
    assert(ts_dhcp_server_apply_eth_network(&host,(uint32_t[]){0,0})==ESP_FAIL);
    assert(stopped_notifications==1&&!started_notifications&&netifs[1].status==ESP_NETIF_DHCP_STOPPED);
    record("running_host_application_failure_notifies_confirmed_stop");

    setup(true);s_state.iface[1].apply_error=ESP_FAIL;
    ts_dhcp_status_t status;assert(ts_dhcp_server_get_status(1,&status)==ESP_OK);
    assert(status.state==TS_DHCP_STATE_RUNNING&&status.apply_error==ESP_FAIL&&ts_dhcp_server_is_running(1));
    for(unsigned api=0;api<3;api++) {
        fail_stage=FAIL_STATUS;
        if(api==0)assert(ts_dhcp_server_get_status(1,&status)==ESP_FAIL);
        if(api==1)assert(ts_dhcp_server_get_state(1)==TS_DHCP_STATE_ERROR);
        if(api==2)assert(!ts_dhcp_server_is_running(1));
    }
    for(unsigned all=0;all<2;all++)for(unsigned json=0;json<2;json++) {
        fail_stage=FAIL_STATUS;console_text[0]=0;
        assert(do_dhcp_status(all?TS_DHCP_IF_ALL:0,json)==1);
        assert(strstr(console_text,json?"error":"Error")&&!strstr(console_text,"stopped")&&!strstr(console_text,"Active Leases:"));
    }
    record("SDK_query_error_propagates_three_public_queries_and_actual_CLI_single_all_JSON");
    prepare_custom_boot();
    ts_net_ip_info_t info={.ip=infos[1].ip.addr,.netmask=infos[1].netmask.addr,.gateway=infos[1].gw.addr,
                           .dns1=inet_addr("9.9.9.9"),.dns2=inet_addr("1.1.1.1")};
    for(unsigned custom=0;custom<2;custom++) {
        if(!custom)s_state.iface[1].config.pool.dns1[0]=0;
        else strcpy(s_state.iface[1].config.pool.dns1,"8.8.8.8");
        memset(dns_writes,0,sizeof(dns_writes));assert(ts_net_set_ip_info(TS_NET_IF_ETH,&info)==ESP_OK);
        assert(dns_writes[0]==1&&dns_writes[1]==1);
        assert(servers[1].dns_server[0].addr==(custom?inet_addr("8.8.8.8"):info.dns1));
        assert(servers[1].dns_server[1].addr==info.dns2);
        assert(option_value(&servers[1],DHCP_OPTION_DNS_SERVER,true)==(custom?inet_addr("8.8.8.8"):info.ip));
    }
    record("DNS_single_application_preserves_host_backup_and_pool_offer_policy");
}

static void check_review_findings(void) {
    setup(true);assert(ts_dhcp_server_save_config()==ESP_OK);
    fail_stage=FAIL_START;assert(ts_dhcp_server_restart(0)==ESP_FAIL);
    assert(ts_dhcp_server_get_restore_error(0)==ESP_OK);
    assert(ts_dhcp_server_get_apply_error(0)==ESP_FAIL);
    assert(do_dhcp_pool(0,NULL,NULL,NULL,NULL,"1.1.1.1",0)==0);
    assert(ts_dhcp_server_get_apply_error(0)==ESP_FAIL); /* Editing is not an application. */
    assert(ts_dhcp_server_start(0)==ESP_OK&&ts_dhcp_server_get_apply_error(0)==ESP_OK);
    record("review1_apply_failure_does_not_force_full_CLI_config_replacement");

    for(unsigned absent_namespace=0;absent_namespace<2;absent_namespace++) {
        setup(true);assert(ts_dhcp_server_save_config()==ESP_OK);fail_read_key=NVS_KEY_AP_GATEWAY;
        assert(ts_dhcp_server_load_config()==ESP_FAIL);fail_read_key=NULL;
        if(absent_namespace)missing_namespace=true;else remove_interface_record(0);
        ts_dhcp_config_t retained=s_state.iface[0].config;
        esp_netif_dhcp_status_t status=netifs[0].status;unsigned dispatches=apply_dispatches;
        s_state.iface[0].apply_error=ESP_FAIL;
        assert(ts_dhcp_server_load_config()==ESP_OK);
        assert(ts_dhcp_server_get_restore_error(0)==ESP_OK&&ts_dhcp_server_get_apply_error(0)==ESP_FAIL);
        assert(same_network_config(&retained,&s_state.iface[0].config)&&netifs[0].status==status);
        assert(apply_dispatches==dispatches+(absent_namespace?0:1)); /* Absent AP is never applied. */
        missing_namespace=false;assert(ts_dhcp_server_save_config()==ESP_OK);
    }
    record("confirmed_absent_interface_or_namespace_clears_only_storage_errors_without_applying_AP");

    boot_defaults(true);
    ts_dhcp_static_binding_t orphan={.mac={2,0,0,0,0,1},.ip="10.10.99.50",.enabled=true};
    assert(nvs_set_blob(0,NVS_KEY_AP_BINDINGS,&orphan,sizeof(orphan))==ESP_OK);
    assert(ts_dhcp_server_load_config()==ESP_ERR_NVS_NOT_FOUND);
    assert(ESP_ERR_NVS_NOT_FOUND!=ESP_ERR_NOT_FOUND);
    assert(ts_dhcp_server_get_restore_error(0)==ESP_ERR_NVS_NOT_FOUND);
    assert(ts_dhcp_server_save_config()==ESP_ERR_NVS_NOT_FOUND);
    uint8_t count=0;assert(nvs_get_u8(0,NVS_KEY_AP_BIND_CNT,&count)==ESP_ERR_NVS_NOT_FOUND);
    ts_dhcp_static_binding_t preserved;size_t len=sizeof(preserved);
    assert(nvs_get_blob(0,NVS_KEY_AP_BINDINGS,&preserved,&len)==ESP_OK&&!memcmp(&preserved,&orphan,sizeof(orphan)));
    record("review3_orphan_blob_is_NVS_error_not_absent_interface_and_not_overwritten");

    boot_defaults(true);interface_up(0);
    assert(ts_dhcp_server_set_lease_time(0,1440)==ESP_OK);
    wifi_event_ap_staconnected_t joined={.mac={2,0,0,0,0,1},.aid=1};
    wifi_event_handler(NULL,WIFI_EVENT,WIFI_EVENT_AP_STACONNECTED,&joined);
    assert(s_state.iface[0].client_count==1);
    assert(s_state.iface[0].clients[0].lease_expire-s_state.iface[0].clients[0].lease_start==7200);
    assert(ntohl(option_value(&servers[0],DHCP_OPTION_LEASE_TIME,true))==7200);
    record("review4_initial_AP_auto_start_client_expiry_matches_SDK_offer_not_pending");

    setup(true);assert(ts_dhcp_server_set_lease_time(1,1440)==ESP_OK);
    servers[1].dhcps_lease_time=3; /* Actual SDK lease is authoritative for statistics. */
    run_lease_event(a,101);
    assert(s_state.iface[1].client_count==1);
    assert(s_state.iface[1].clients[0].lease_expire-s_state.iface[1].clients[0].lease_start==180);
    run_lease_event(a,102);assert(s_state.iface[1].client_count==1);
    assert(s_state.iface[1].clients[0].lease_expire-s_state.iface[1].clients[0].lease_start==180);
    record("review4_ETH_new_and_renewed_client_expiry_use_active_lease_not_pending");

    setup(true);assert(ts_dhcp_server_set_lease_time(0,1440)==ESP_OK);
    fail_stage=FAIL_LEASE_READ;wifi_event_handler(NULL,WIFI_EVENT,WIFI_EVENT_AP_STACONNECTED,&joined);
    assert(s_state.iface[0].clients[0].lease_expire==0);
    record("review4_active_lease_read_error_is_unknown_without_pending_fallback");
}
static void check_fault_boundaries(void) {
    boot_defaults(true);assert(ts_dhcp_server_load_config()==ESP_OK);
    assert(!apply_dispatches&&netifs[0].status==ESP_NETIF_DHCP_INIT);
    record("no_saved_record_preserves_default_auto_start_without_configure");
    netifs_exist=false;
    assert(bind_owner_if(0,a,50,true)==ESP_ERR_INVALID_ARG);
    assert(s_state.iface[0].static_binding_count==0);
    netifs_exist=true;
    record("AP_without_netif_validates_staged_subnet_before_binding_commit");

    setup(true);assert(bind_owner_if(0,a,50,true)==ESP_OK);assert(ts_dhcp_server_save_config()==ESP_OK);
    boot_defaults(false);fail_stage=FAIL_BINDINGS;assert(ts_dhcp_server_load_config()!=ESP_OK);
    assert(s_state.iface[0].static_binding_count==0);
    assert(ts_dhcp_server_save_config()!=ESP_OK);
    uint8_t saved=0;assert(nvs_get_u8(0,NVS_KEY_AP_BIND_CNT,&saved)==ESP_OK&&saved==1);
    assert(ts_dhcp_server_start(0)!=ESP_OK); /* Cannot silently adopt the empty fallback. */
    assert(ts_dhcp_server_load_config()==ESP_OK);assert(s_state.iface[0].static_binding_count==1);
    record("boot_install_failure_cannot_save_or_start_as_empty_then_reload_recovers");

    setup(true);assert(ts_dhcp_server_save_config()==ESP_OK);assert(nvs_set_str(0,NVS_KEY_AP_BIND_CNT,"bad")==ESP_OK);
    assert(ts_dhcp_server_load_config()==ESP_ERR_NVS_TYPE_MISMATCH);
    assert(ts_dhcp_server_save_config()!=ESP_OK);
    record("NVS_wrong_type_is_error_not_missing_or_empty");

    setup(true);assert(bind_owner_if(0,a,50,true)==ESP_OK);assert(ts_dhcp_server_save_config()==ESP_OK);
    boot_defaults(false);fail_read_key=NVS_KEY_AP_GATEWAY;assert(ts_dhcp_server_load_config()!=ESP_OK);fail_read_key=NULL;
    ts_dhcp_config_t replacement;
    set_interface_defaults(0,&replacement);assert(ts_dhcp_server_set_config(0,&replacement)==ESP_OK);
    assert(ts_dhcp_server_save_config()!=ESP_OK);assert(bind_owner_if(0,b,102,true)!=ESP_OK);
    uint8_t original=0;assert(nvs_get_u8(0,NVS_KEY_AP_BIND_CNT,&original)==ESP_OK&&original==1);
    assert(ts_dhcp_server_load_config()==ESP_OK&&s_state.iface[0].static_binding_count==1);
    record("read_successful_but_uncommitted_bindings_remain_save_protected");

    setup(true);assert(bind_owner_if(0,a,50,true)==ESP_OK);assert(ts_dhcp_server_save_config()==ESP_OK);
    assert(shrink_blob(NVS_KEY_AP_BINDINGS,1));boot_defaults(false);
    assert(ts_dhcp_server_load_config()!=ESP_OK);assert(ts_dhcp_server_clear_static_bindings(0)==ESP_OK);
    assert(ts_dhcp_server_save_config()!=ESP_OK); /* clear cannot replace unread/unapplied custom config. */
    record("read_successful_but_uncommitted_config_not_unlocked_by_clear");


    setup(true);assert(ts_dhcp_server_save_config()==ESP_OK);fail_read_key=NVS_KEY_AP_GATEWAY;
    assert(ts_dhcp_server_load_config()==ESP_FAIL);fail_read_key=NULL;
    assert(do_dhcp_pool(0,NULL,NULL,NULL,NULL,"8.8.8.8",0)==1);
    assert(ts_dhcp_server_get_restore_error(0)!=ESP_OK);
    assert(do_dhcp_pool(0,"10.10.99.100","10.10.99.103","10.10.99.97","255.255.255.0","",120)==0);
    /* Full config can only clear its own protection, not the binding error. */
    assert(ts_dhcp_server_get_restore_error(0)==ESP_OK);
    record("CLI_partial_DNS_edit_cannot_unlock_unrestored_config_full_replace_can");

    setup(true);assert(bind_owner_if(0,a,101,true)==ESP_OK);
    for(unsigned scenario=0;scenario<5;scenario++) {
        cli_defaults();s_dhcp_args.clear_bindings->count=1;
        const char *name=scenario==0?"ap":scenario==1?"eth":scenario==2?"all":"invalid";
        s_dhcp_args.iface->sval=&name;s_dhcp_args.iface->count=scenario==4?0:1;
        if(scenario<2) {
            unsigned before=commits;assert(do_cmd_dhcp(0,NULL)==0);assert(commits==before);
        } else assert(do_cmd_dhcp(0,NULL)==1);
    }
    cli_defaults();const char *name="ap";s_dhcp_args.iface->sval=&name;s_dhcp_args.iface->count=1;
    s_dhcp_args.clear_bindings->count=1;s_dhcp_args.save->count=1;assert(do_cmd_dhcp(0,NULL)==1);
    record("actual_CLI_dispatch_clear_requires_single_explicit_iface_and_no_save");

    setup(true);assert(ts_dhcp_server_save_config()==ESP_OK);
    assert(nvs_set_u32(0,NVS_KEY_AP_LEASE,2)==ESP_OK);fail_stage=FAIL_START;fail_cleanup_stop=true;
    assert(ts_dhcp_server_load_config()==ESP_FAIL);assert(s_state.iface[0].apply_error==ESP_FAIL);
    assert(netifs[0].status==ESP_NETIF_DHCP_INIT); /* Report actual SDK state, not fictional stopped. */
    record("startup_and_cleanup_failure_reports_error_without_claiming_stopped");
}
static void check_lifecycle(void) {
    /* P2 old implementation drops this valid reservation against default AP IP. */
    setup(true);assert(bind_owner_if(0,a,50,true)==ESP_OK);assert(ts_dhcp_server_save_config()==ESP_OK);
    boot_defaults(false);assert(ts_dhcp_server_load_config()==ESP_OK);
    assert(netifs[0].status==ESP_NETIF_DHCP_INIT&&servers[0].ts_bindings.count==1);
    interface_up(0);assert(netifs[0].status==ESP_NETIF_DHCP_STARTED);
    assert(packet(&servers[0],a,DHCPDISCOVER,0,0,DHCPS_STATE_OFFER)==50);
    record("P2_saved_subnet_reboot_first_offer_has_owner");

    /* P1 f43 leaves STOPPED, so a queued AP_START cannot start DHCP. */
    setup(true);assert(bind_owner_if(0,a,50,true)==ESP_OK);assert(ts_dhcp_server_save_config()==ESP_OK);
    boot_defaults(false);after_dispatch=queued_ap_up;
    assert(ts_dhcp_server_load_config()==ESP_OK);
    assert(netifs[0].status==ESP_NETIF_DHCP_STARTED&&servers[0].ts_bindings.count==1);
    assert(packet(&servers[0],a,DHCPDISCOVER,0,0,DHCPS_STATE_OFFER)==50);
    record("P1_interface_up_queued_after_atomic_apply_starts_with_binding");

    /* Every SDK state x up/down on both interfaces; full reconfigure is forced. */
    for(unsigned iface=0;iface<2;iface++)for(unsigned up=0;up<2;up++)for(unsigned state=0;state<3;state++) {
        setup(true);assert(ts_dhcp_server_save_config()==ESP_OK);
        s_state.iface[iface].applied_valid=false;netifs[iface].up=up;netifs[iface].status=state;s_link_up=up;
        assert(ts_dhcp_server_load_config()==ESP_OK);
        unsigned expected=state==ESP_NETIF_DHCP_STOPPED?ESP_NETIF_DHCP_STOPPED:
            up?ESP_NETIF_DHCP_STARTED:ESP_NETIF_DHCP_INIT;
        if(iface==TS_DHCP_IF_ETH) expected=up?ESP_NETIF_DHCP_STARTED:ESP_NETIF_DHCP_STOPPED;
        assert(netifs[iface].status==expected);
    }
    record("lifecycle_both_interfaces_three_sdk_states_up_down");

    /* New and legacy ETH-only saves must not poison AP on reboot. */
    boot_defaults(true);assert(ts_dhcp_server_save_config()==ESP_OK);
    boot_defaults(false);assert(ts_dhcp_server_load_config()==ESP_OK);interface_up(0);
    assert(TS_DHCP_AP_DEFAULT_LEASE_TIME==DHCPS_LEASE_TIME_DEF);
    unsigned first=ntohl(servers[0].dhcps_poll.start_ip.addr),last=ntohl(servers[0].dhcps_poll.end_ip.addr);
    assert(first==ntohl(inet_addr("192.168.4.1"))+1&&last-first+1==DHCPS_MAX_LEASE);
    assert(last==ntohl(inet_addr("192.168.4.101")));
    assert(ntohl(option_value(&servers[0],DHCP_OPTION_LEASE_TIME,true))==7200);
    assert(option_value(&servers[0],DHCP_OPTION_DNS_SERVER,true)==inet_addr("192.168.4.1"));
    assert(!option_value(&servers[0],DHCP_OPTION_DNS_SERVER,false));
    record("AP_defaults_saved_reboot_real_SDK_offer_pool_lease_DNS");

    boot_defaults(true);assert(ts_dhcp_server_save_config()==ESP_OK);
    ts_dhcp_config_t legacy;set_default_config(&legacy);
    const char *values[]={legacy.pool.start_ip,legacy.pool.end_ip,legacy.pool.gateway,legacy.pool.netmask,legacy.pool.dns1};
    for(unsigned i=0;i<5;i++)set_saved_string(0,i+1,values[i]);
    assert(nvs_set_u32(0,NVS_KEY_AP_LEASE,legacy.lease_time_min)==ESP_OK);
    unsigned before=commits;boot_defaults(false);assert(ts_dhcp_server_load_config()==ESP_OK);assert(commits==before);
    assert(!strcmp(s_state.iface[0].config.pool.gateway,"192.168.4.1"));
    assert(ts_dhcp_server_get_restore_error(0)==ESP_OK);
    interface_up(0);assert(option_value(&servers[0],DHCP_OPTION_DNS_SERVER,true)==inet_addr("192.168.4.1"));
    assert(ts_dhcp_server_save_config()==ESP_OK);boot_defaults(false);assert(ts_dhcp_server_load_config()==ESP_OK);
    record("legacy_default_empty_AP_compat_no_boot_write_then_explicit_save");

    /* Old defaults with nonempty bindings may never be treated as unconfigured. */
    setup(true);assert(bind_owner_if(0,a,101,true)==ESP_OK);assert(ts_dhcp_server_save_config()==ESP_OK);
    for(unsigned i=0;i<5;i++)set_saved_string(0,i+1,values[i]);
    assert(nvs_set_u32(0,NVS_KEY_AP_LEASE,legacy.lease_time_min)==ESP_OK);
    boot_defaults(false);assert(ts_dhcp_server_load_config()!=ESP_OK);
    assert(ts_dhcp_server_get_restore_error(0)!=ESP_OK);
    before=commits;assert(ts_dhcp_server_save_config()!=ESP_OK);assert(commits==before+1);
    record("legacy_nonempty_AP_not_compat_protected_ETH_still_saved");

    setup(true);strcpy(s_state.iface[0].config.pool.dns1,"8.8.8.8");
    assert(ts_dhcp_server_restart(0)==ESP_OK);
    assert(option_value(&servers[0],DHCP_OPTION_DNS_SERVER,true)==inet_addr("8.8.8.8"));
    s_state.iface[0].config.pool.dns1[0]=0;assert(ts_dhcp_server_restart(0)==ESP_OK);
    assert(!servers[0].dhcps_dns);
    assert(option_value(&servers[0],DHCP_OPTION_DNS_SERVER,true)==ip(97).addr);
    assert(!option_value(&servers[0],DHCP_OPTION_DNS_SERVER,false));
    assert(ts_dhcp_server_save_config()==ESP_OK);boot_defaults(false);assert(ts_dhcp_server_load_config()==ESP_OK);interface_up(0);
    assert(option_value(&servers[0],DHCP_OPTION_DNS_SERVER,true)==ip(97).addr);
    record("custom_DNS_to_empty_explicit_disable_save_reboot_SDK_fallback");

    /* A pending new subnet must be rejected before restart can stop old service. */
    setup(true);assert(bind_owner_if(0,a,101,true)==ESP_OK);
    ts_dhcp_config_t pending=s_state.iface[0].config;
    strcpy(pending.pool.gateway,"192.168.7.1");strcpy(pending.pool.start_ip,"192.168.7.2");strcpy(pending.pool.end_ip,"192.168.7.20");
    assert(ts_dhcp_server_set_config(0,&pending)==ESP_OK);
    ts_dhcp_status_t live={0};assert(ts_dhcp_server_get_status(0,&live)==ESP_OK&&live.total_pool_size==4);
    assert(bind_owner_if(0,b,102,true)==ESP_OK);unsigned stopped=stop_calls;
    assert(ts_dhcp_server_restart(0)==ESP_ERR_INVALID_ARG);assert(stop_calls==stopped);
    assert(netifs[0].status==ESP_NETIF_DHCP_STARTED&&servers[0].ts_bindings.count==2);
    record("pending_config_revalidates_full_table_before_stop");

    /* Save and editing may never turn an unread table into an authoritative zero. */
    setup(true);assert(bind_owner_if(0,a,101,true)==ESP_OK);assert(ts_dhcp_server_save_config()==ESP_OK);
    assert(shrink_blob(NVS_KEY_AP_BINDINGS,1));assert(ts_dhcp_server_load_config()==ESP_ERR_INVALID_SIZE);
    size_t stored_count=0;uint8_t count=0;stored_count=sizeof(count);assert(kv_get(NVS_KEY_AP_BIND_CNT,&count,&stored_count)==ESP_OK&&count==1);
    assert(bind_owner_if(0,b,102,true)!=ESP_OK);assert(ts_dhcp_server_save_config()!=ESP_OK);
    stored_count=sizeof(count);assert(kv_get(NVS_KEY_AP_BIND_CNT,&count,&stored_count)==ESP_OK&&count==1);
    assert(do_dhcp_clear_bindings(TS_DHCP_IF_ALL)==1);assert(do_dhcp_clear_bindings(0)==0);
    stored_count=sizeof(count);assert(kv_get(NVS_KEY_AP_BIND_CNT,&count,&stored_count)==ESP_OK&&count==1);
    assert(ts_dhcp_server_save_config()==ESP_OK);stored_count=sizeof(count);assert(kv_get(NVS_KEY_AP_BIND_CNT,&count,&stored_count)==ESP_OK&&count==0);
    record("restore_failed_table_save_protected_clear_is_explicit_and_not_saved");

    for(enum failure_stage stage=FAIL_STATUS;stage<=FAIL_START;stage++) {
        setup(true);assert(bind_owner_if(0,a,101,true)==ESP_OK);assert(ts_dhcp_server_save_config()==ESP_OK);
        assert(nvs_set_u32(0,NVS_KEY_AP_LEASE,2)==ESP_OK);set_saved_string(0,5,"1.1.1.1");
        ts_dhcps_binding_table_t old=servers[0].ts_bindings;fail_stage=stage;
        assert(ts_dhcp_server_load_config()!=ESP_OK);
        if(stage==FAIL_STATUS||stage==FAIL_STOP)assert(netifs[0].status==ESP_NETIF_DHCP_STARTED);
        else assert(netifs[0].status==ESP_NETIF_DHCP_STOPPED);
        assert(!memcmp(&old,&servers[0].ts_bindings,sizeof(old)));
        if(stage==FAIL_START)assert(s_state.iface[0].config.lease_time_min==2);
        else assert(s_state.iface[0].config.lease_time_min==1);
    }
    record("SDK_each_apply_stage_failure_reports_and_disarms_partial_service");

    setup(true);assert(ts_dhcp_server_save_config()==ESP_OK);fail_read_key=NVS_KEY_AP_GATEWAY;
    assert(ts_dhcp_server_load_config()==ESP_FAIL);assert(netifs[0].status==ESP_NETIF_DHCP_STARTED);
    fail_read_key=NULL;assert(ts_dhcp_server_save_config()!=ESP_OK);assert(ts_dhcp_server_load_config()==ESP_OK);
    assert(ts_dhcp_server_save_config()==ESP_OK);
    fail_write_key=NVS_KEY_AP_GATEWAY;assert(ts_dhcp_server_save_config()==ESP_FAIL);fail_write_key=NULL;
    fail_commit=true;assert(ts_dhcp_server_save_config()==ESP_FAIL);fail_commit=false;
    record("NVS_read_write_commit_errors_and_retry_recovery");
}
int main(void) {
    check_transitions_and_queries();
    check_eth_lifecycle();
    check_lifecycle();
    check_fault_boundaries();
    check_review_findings();
    setup(true);assert(discover(a)==100);request(a,100);advance(30);renew(a,100);
    assert(lease(a,100)->lease_timer==60);advance(59);assert(lease(a,100));advance(1);assert(!eth()->plist);
    assert(discover(b)==100);request(b,100);release(b,100);assert(!eth()->plist);record("A_dynamic_discover_request_renew_expire_release_reuse");

    setup(true);assert(do_dhcp_bind(1,"02:00:00:00:00:01","10.10.99.101","owner")==0);
    assert(strstr(console_text,"Static binding added")&&commits==0);assert(discover(a)==101);request(a,101);advance(30);renew(a,101);
    advance(60);assert(!eth()->plist&&eth()->ts_bindings.count==1);
    assert(discover(b)==100);request(b,100);assert(discover(c)==102);request(c,102);assert(discover(a)==101);request(a,101);
    record("B_owner_offline_past_lease_others_skip101_owner_returns101");
    release(a,101);assert(!lease(a,101));
    packet(eth(),dmac,DHCPREQUEST,0,101,DHCPS_STATE_NAK);assert(!lease(dmac,101));
    packet(eth(),dmac,DHCPREQUEST,101,0,DHCPS_STATE_NAK);assert(!lease(dmac,101));
    assert(discover(a)==101);request(a,101);release(a,101);
    assert(packet(eth(),a,DHCPREQUEST,0,101,DHCPS_STATE_ACK)==101);
    record("C_release_preserves_owner_direct_request_reconnect_and_denies_other_MAC");

    setup(true);assert(bind_owner(a,101,true)==ESP_OK);assert(discover(a)==101);request(a,101);advance(10);
    assert(bind_owner(a,102,true)==ESP_OK);assert(lease(a,101)&&lease(a,101)->lease_timer==50);
    packet(eth(),a,DHCPREQUEST,101,0,DHCPS_STATE_NAK);assert(lease(a,101)&&!lease(a,102));
    assert(discover(a)==102);request(a,102);renew(a,102);assert(lease(a,101)&&lease(a,102));
    release(a,101);assert(!lease(a,101)&&lease(a,102));release(a,102);assert(discover(b)==100);assert(discover(c)==101);
    assert(discover(a)==102);record("D_rebind_next_exchange_old_renew_NAK_old_RELEASE_new_lease_preserved");

    setup(true);assert(bind_owner(a,101,true)==ESP_OK);assert(discover(a)==101);request(a,101);
    assert(bind_owner(a,102,true)==ESP_OK);assert(discover(a)==102);request(a,102);
    assert(ts_dhcp_server_remove_static_binding(1,a)==ESP_OK);renew(a,102);
    assert(lease(a,101)&&lease(a,102));record("D_rebind_then_remove_renew_current_lease_without_disconnect");

    for(int removal=0;removal<3;removal++) {
        setup(true);assert(bind_owner(a,101,true)==ESP_OK);assert(discover(a)==101);request(a,101);
        if(removal==0)assert(ts_dhcp_server_remove_static_binding(1,a)==ESP_OK);
        if(removal==1)assert(bind_owner(a,101,false)==ESP_OK);
        if(removal==2)assert(ts_dhcp_server_clear_static_bindings(1)==ESP_OK);
        assert(eth()->ts_bindings.count==0&&lease(a,101));renew(a,101);advance(60);assert(!eth()->plist);
        assert(discover(b)==100);assert(discover(c)==101);assert(bind_owner(a,102,true)==ESP_OK);
        assert(discover(a)==102);record(removal==0?"E_remove_keeps_live_lease_then_reuses":removal==1?"E_disable_keeps_live_lease_then_reuses":"E_clear_zero_table_keeps_live_lease_then_reuses");
    }
    const unsigned outside[]={50,200};
    for(size_t i=0;i<sizeof(outside)/sizeof(*outside);i++) {
        setup(true);unsigned last=outside[i];assert(bind_owner(a,last,true)==ESP_OK);assert(discover(a)==last);request(a,last);
        assert(ts_dhcp_server_remove_static_binding(1,a)==ESP_OK);assert(lease(a,last));renew(a,last);assert(lease(a,last));
        release(a,last);assert(!lease(a,last));
        char name[80];snprintf(name,sizeof(name),"E_remove_outside_pool_%u_keeps_live_renewal_until_release",last);record(name);
    }
    const unsigned addresses[]={101,50,100,200};
    for(size_t i=0;i<sizeof(addresses)/sizeof(*addresses);i++) {
        unsigned last=addresses[i];setup(true);assert(bind_owner(a,last,true)==ESP_OK);assert(commits==0);
        assert(ts_dhcp_server_save_config()==ESP_OK&&commits==2);
        setup(false);netifs_exist=false;assert(ts_dhcp_server_load_config()==ESP_OK);assert(eth()->ts_bindings.count==0);
        assert(s_state.iface[1].static_binding_count==1);netifs_exist=true;start_expected_owner=last;check_start_table=true;
        assert(ts_dhcp_server_start(1)==ESP_OK);assert(discover(a)==last);request(a,last);
        unsigned other=discover(b);assert(other>=100&&other<=103&&other!=last);
        char name[80];snprintf(name,sizeof(name),"F_saved_%u_load_before_netif_start_installed_before_first_offer",last);record(name);
    }
    /* Both interfaces x count overflow/short/long blob: preserve complete tables. */
    for(ts_dhcp_if_t iface=0;iface<TS_DHCP_IF_MAX;iface++)for(unsigned damage=0;damage<3;damage++) {
        setup(true);assert(bind_owner_if(iface,a,101,true)==ESP_OK);
        if(damage==2)assert(bind_owner_if(iface,dmac,102,true)==ESP_OK);
        assert(ts_dhcp_server_save_config()==ESP_OK);
        ts_dhcp_static_binding_t before[TS_DHCP_MAX_STATIC_BINDINGS];size_t count;
        assert(ts_dhcp_server_get_static_bindings(iface,before,TS_DHCP_MAX_STATIC_BINDINGS,&count)==ESP_OK);
        ts_dhcps_binding_table_t kept=servers[iface].ts_bindings;
        const char *const *keys=dhcp_nvs_keys(iface);
        if(damage==1)assert(shrink_blob(keys[8],1));
        else assert(nvs_set_u8(0,keys[7],damage==0?TS_DHCP_MAX_STATIC_BINDINGS+1:1)==ESP_OK);
        assert(ts_dhcp_server_load_config()==(damage==2?ESP_ERR_NVS_INVALID_LENGTH:ESP_ERR_INVALID_SIZE));
        assert(!memcmp(&kept,&servers[iface].ts_bindings,sizeof(kept)));
        assert(s_state.iface[iface].static_binding_count==count);
        assert(!memcmp(before,s_state.iface[iface].static_bindings,count*sizeof(*before)));
    }
    record("NVS_both_interfaces_count_capacity_short_and_long_blob_preserve_complete_tables");

    /* AP data must not reach ETH when ETH's own record is inconsistent. */
    setup(true);
    ts_dhcp_static_binding_t ap={.enabled=true};memcpy(ap.mac,c,6);strcpy(ap.ip,"10.10.99.102");
    assert(ts_dhcp_server_add_static_binding(TS_DHCP_IF_AP,&ap)==ESP_OK);
    assert(bind_owner_if(TS_DHCP_IF_ETH,a,101,true)==ESP_OK);assert(ts_dhcp_server_save_config()==ESP_OK);
    ts_dhcps_binding_table_t eth_kept=servers[1].ts_bindings;size_t eth_kept_count=s_state.iface[1].static_binding_count;
    assert(shrink_blob(NVS_KEY_ETH_BINDINGS,1));
    assert(ts_dhcp_server_load_config()==ESP_ERR_INVALID_SIZE);
    assert(!memcmp(&eth_kept,&servers[1].ts_bindings,sizeof(eth_kept))&&s_state.iface[1].static_binding_count==eth_kept_count);
    for(size_t i=0;i<servers[1].ts_bindings.count;i++)assert(servers[1].ts_bindings.entries[i].ip!=ip(102).addr);
    assert(s_state.iface[0].static_binding_count==1&&servers[0].ts_bindings.count==1);
    record("F3_ap_candidate_does_not_leak_into_eth");

    setup(true);assert(ts_dhcp_server_save_config()==ESP_OK);assert(bind_owner(a,101,true)==ESP_OK);
    assert(ts_dhcp_server_load_config()==ESP_OK);assert(eth()->ts_bindings.count==0&&s_state.iface[1].static_binding_count==0);
    assert(discover(b)==100);assert(discover(c)==101);record("F_loading_saved_empty_table_clears_old_ownership");

    setup(true);assert(bind_owner(a,102,true)==ESP_OK);assert(discover(b)==100);request(b,100);
    ts_dhcp_static_binding_t before[16];size_t count;
    assert(ts_dhcp_server_get_static_bindings(1,before,16,&count)==ESP_OK&&count==1);
    ts_dhcps_binding_table_t old_table=eth()->ts_bindings;
    struct dhcps_pool old_lease=*lease(b,100);
    assert(bind_owner(a,100,true)==ESP_ERR_INVALID_STATE);
    assert(!memcmp(&old_table,&eth()->ts_bindings,sizeof(old_table))&&!memcmp(&old_lease,lease(b,100),sizeof(old_lease)));
    assert(s_state.iface[1].static_binding_count==count&&!memcmp(before,s_state.iface[1].static_bindings,count*sizeof(*before)));
    assert(bind_owner(c,102,true)==ESP_ERR_INVALID_STATE&&s_state.iface[1].static_binding_count==1);
    assert(do_dhcp_bind(1,"02:00:00:00:00:01","10.10.99.100",NULL)==1);
    assert(strstr(console_text,"Address reserved or leased")&&strstr(console_text,"unchanged"));
    assert(discover(a)==102);renew(b,100);record("G_live_lease_and_binding_conflicts_fail_preserve_memory_table_lease_CLI_reason");
    assert(bind_owner(c,103,false)==ESP_OK);assert(bind_owner(dmac,103,true)==ESP_OK);
    assert(bind_owner(c,103,true)==ESP_ERR_INVALID_STATE);record("G_enabling_disabled_binding_conflict_keeps_disabled");

    setup(true);assert(bind_owner(a,101,true)==ESP_OK);assert(ts_dhcp_server_save_config()==ESP_OK);
    assert(ts_dhcp_server_clear_static_bindings(1)==ESP_OK);assert(discover(b)==100);assert(discover(c)==101);request(c,101);
    assert(bind_owner(a,102,true)==ESP_OK);old_table=eth()->ts_bindings;old_lease=*lease(c,101);
    assert(ts_dhcp_server_load_config()==ESP_ERR_INVALID_STATE);
    assert(!memcmp(&old_table,&eth()->ts_bindings,sizeof(old_table))&&!memcmp(&old_lease,lease(c,101),sizeof(old_lease)));
    assert(!strcmp(s_state.iface[1].static_bindings[0].ip,"10.10.99.102"));record("G_NVS_reload_conflict_preserves_previous_memory_owner_and_lease");

    setup(true);assert(bind_owner(a,100,true)==ESP_OK);assert(bind_owner(b,101,true)==ESP_OK);
    assert(bind_owner(c,102,true)==ESP_OK);assert(bind_owner(dmac,103,true)==ESP_OK);
    packet(eth(),e,DHCPDISCOVER,0,0,DHCPS_STATE_NAK);assert(!eth()->plist);
    assert(discover(c)==102);request(c,102);advance(60);assert(!eth()->plist&&eth()->ts_bindings.count==4);
    assert(discover(c)==102);record("H_full_reserved_pool_denies_dynamic_owner_still_works_after_expiry");
    assert(ipc_calls>0&&!project_lock_depth);
    for(int i=0;i<TS_DHCP_IF_MAX;i++)drain(&servers[i]);
    return 0;
}
