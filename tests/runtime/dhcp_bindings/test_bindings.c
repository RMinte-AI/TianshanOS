#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <pthread.h>
#include "esp_err.h"
#include "esp_netif.h"
#include "dhcpserver/dhcpserver.h"
#include "ts_dhcp_server.h"
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
#include "bridge_core.inc"

typedef pthread_mutex_t *SemaphoreHandle_t;
typedef void *QueueHandle_t;
typedef void *TaskHandle_t;
typedef unsigned TickType_t;
#define portMAX_DELAY -1
#define NVS_READWRITE 1
#define NVS_READONLY 0
#define IPSTR "%u"
#define IP2STR(p) (p)->addr
#define ESP_NETIF_OP_SET 0
#define ESP_NETIF_REQUESTED_IP_ADDRESS 1
#define ESP_NETIF_IP_ADDRESS_LEASE_TIME 2
#define ESP_NETIF_DOMAIN_NAME_SERVER 3
#define ESP_IPADDR_TYPE_V4 0
#define ESP_NETIF_DNS_MAIN 0
#define TS_DHCP_IF_ALL TS_DHCP_IF_MAX
static pthread_mutex_t project_mutex=PTHREAD_MUTEX_INITIALIZER;
static unsigned project_lock_depth;
static void xSemaphoreTake(SemaphoreHandle_t mutex,int ticks) {
    assert(!on_tcpip&&!project_lock_depth);assert(!pthread_mutex_lock(mutex));project_lock_depth++;
}
static void xSemaphoreGive(SemaphoreHandle_t mutex) {assert(project_lock_depth==1);project_lock_depth--;assert(!pthread_mutex_unlock(mutex));}
#include "project_types.inc"
static ts_dhcp_module_state_t s_state;
static dhcps_t servers[TS_DHCP_IF_MAX];
static esp_netif_ip_info_t infos[TS_DHCP_IF_MAX];
static esp_netif_t netifs[TS_DHCP_IF_MAX];
static bool netifs_exist=true;
static unsigned start_expected_owner;
static bool check_start_table;
static ip4_addr_t ip(unsigned last) {return (ip4_addr_t){htonl(0x0a0a6300U+last)};}
static void drain(dhcps_t *d) {while(d->plist){list_node *n=d->plist;d->plist=n->pnext;free(n->pnode);free(n);}}
static esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key) {
    if(!netifs_exist)return NULL;
    return !strcmp(key,"WIFI_AP_DEF")?&netifs[0]:&netifs[1];
}
static esp_err_t esp_netif_dhcps_stop(esp_netif_t *n) {drain(n->dhcps);n->dhcps->state=DHCPS_HANDLE_STOPPED;return ESP_OK;}
static esp_err_t esp_netif_set_ip_info(esp_netif_t *n,const esp_netif_ip_info_t *i) {*n->ip_info=*i;return ESP_OK;}
static esp_err_t esp_netif_get_ip_info(esp_netif_t *n,esp_netif_ip_info_t *i) {*i=*n->ip_info;return ESP_OK;}
static esp_err_t esp_netif_dhcps_option(esp_netif_t *n,int op,int option,void *value,size_t len) {
    if(option==ESP_NETIF_REQUESTED_IP_ADDRESS)n->dhcps->dhcps_poll=*(dhcps_lease_t *)value;
    if(option==ESP_NETIF_IP_ADDRESS_LEASE_TIME)n->dhcps->dhcps_lease_time=*(uint32_t *)value;
    return ESP_OK;
}
typedef struct {struct {struct {ip4_addr_t ip4;} u_addr;int type;} ip;} esp_netif_dns_info_t;
static esp_err_t esp_netif_set_dns_info(esp_netif_t *n,int kind,esp_netif_dns_info_t *dns) {return ESP_OK;}
static esp_err_t esp_netif_dhcps_start(esp_netif_t *n) {
    if(check_start_table) {
        assert(n->dhcps->ts_bindings.count==(start_expected_owner?1:0));
        if(start_expected_owner)assert(n->dhcps->ts_bindings.entries[0].ip==ip(start_expected_owner).addr);
    }
    n->dhcps->state=DHCPS_HANDLE_STARTED;n->dhcps->client_address_plus=n->dhcps->dhcps_poll.start_ip;
    return ESP_OK;
}
static TickType_t xTaskGetTickCount(void) {return 100;}
static void notify_event(ts_dhcp_if_t iface,ts_dhcp_event_t e,const ts_dhcp_client_t *client) {}
const char *ts_dhcp_if_to_str(ts_dhcp_if_t iface) {return iface?"ethernet":"wifi_ap";}
/* NVS key/value model runs the production explicit save and load functions. */
typedef int nvs_handle_t;
static struct {char key[16];unsigned char data[2048];size_t size;} kv[32];
static size_t kv_count;
static unsigned commits;
static esp_err_t nvs_open(const char *name,int mode,nvs_handle_t *handle) {*handle=1;return ESP_OK;}
static void nvs_close(nvs_handle_t handle) {}
static esp_err_t nvs_commit(nvs_handle_t handle) {commits++;return ESP_OK;}
static esp_err_t kv_set(const char *key,const void *data,size_t size) {
    size_t i=0;while(i<kv_count&&strcmp(kv[i].key,key))i++;
    if(i==kv_count){assert(kv_count<32);strcpy(kv[kv_count++].key,key);}
    assert(size<=sizeof(kv[i].data));memcpy(kv[i].data,data,size);kv[i].size=size;return ESP_OK;
}
static esp_err_t kv_get(const char *key,void *data,size_t *size) {
    size_t i=0;while(i<kv_count&&strcmp(kv[i].key,key))i++;
    if(i==kv_count)return ESP_ERR_NVS_NOT_FOUND;
    if(*size<kv[i].size){*size=kv[i].size;return ESP_ERR_NVS_INVALID_LENGTH;}
    memcpy(data,kv[i].data,kv[i].size);*size=kv[i].size;return ESP_OK;
}
/* Simulates a stored record whose count and blob length disagree. */
static bool shrink_blob(const char *key,size_t by) {
    for(size_t i=0;i<kv_count;i++)if(!strcmp(kv[i].key,key)){assert(kv[i].size>by);kv[i].size-=by;return true;}
    return false;
}
static esp_err_t nvs_set_u8(int h,const char *key,uint8_t value) {return kv_set(key,&value,sizeof(value));}
static esp_err_t nvs_set_u32(int h,const char *key,uint32_t value) {return kv_set(key,&value,sizeof(value));}
static esp_err_t nvs_set_str(int h,const char *key,const char *value) {return kv_set(key,value,strlen(value)+1);}
static esp_err_t nvs_set_blob(int h,const char *key,const void *value,size_t size) {return kv_set(key,value,size);}
static esp_err_t nvs_get_u8(int h,const char *key,uint8_t *value) {size_t n=sizeof(*value);return kv_get(key,value,&n);}
static esp_err_t nvs_get_u32(int h,const char *key,uint32_t *value) {size_t n=sizeof(*value);return kv_get(key,value,&n);}
static esp_err_t nvs_get_str(int h,const char *key,char *value,size_t *size) {return kv_get(key,value,size);}
static esp_err_t nvs_get_blob(int h,const char *key,void *value,size_t *size) {return kv_get(key,value,size);}
#include "project_core.inc"
static char console_text[1024];
static const char *esp_err_to_name(esp_err_t err) {return "mock error";}
static void ts_console_printf(const char *fmt,...) {
    size_t n=strlen(console_text);va_list args;va_start(args,fmt);vsnprintf(console_text+n,sizeof(console_text)-n,fmt,args);va_end(args);
}
#include "console_core.inc"

static const uint8_t a[6]={2,0,0,0,0,1},b[6]={2,0,0,0,0,2},c[6]={2,0,0,0,0,3},dmac[6]={2,0,0,0,0,4},e[6]={2,0,0,0,0,5};
static dhcps_t *eth(void) {return &servers[1];}
static void setup(bool erase_nvs) {
    assert(!project_lock_depth);
    for(int i=0;i<TS_DHCP_IF_MAX;i++){drain(&servers[i]);memset(&servers[i],0,sizeof(servers[i]));}
    memset(&s_state,0,sizeof(s_state));s_state.initialized=true;s_state.mutex=&project_mutex;
    for(int i=0;i<TS_DHCP_IF_MAX;i++) {
        set_default_config(&s_state.iface[i].config);
        /* One-minute host lease: no production clock or defaults are changed. */
        s_state.iface[i].config.lease_time_min=1;
        infos[i]=(esp_netif_ip_info_t){.ip=ip(97),.netmask={htonl(0xffffff00)},.gw=ip(100)};
        netifs[i]=(esp_netif_t){&servers[i],&infos[i]};
        servers[i].dhcps_poll=(dhcps_lease_t){true,ip(100),ip(103)};
        servers[i].client_address_plus=ip(100);servers[i].dhcps_lease_time=1;servers[i].state=DHCPS_HANDLE_STARTED;
    }
    netifs_exist=true;check_start_table=false;console_text[0]=0;
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
int main(void) {
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
    for(size_t i=0;i<2;i++) {
        setup(true);unsigned last=outside[i];assert(bind_owner(a,last,true)==ESP_OK);assert(discover(a)==last);request(a,last);
        assert(ts_dhcp_server_remove_static_binding(1,a)==ESP_OK);assert(lease(a,last));renew(a,last);assert(lease(a,last));
        release(a,last);assert(!lease(a,last));
        char name[80];snprintf(name,sizeof(name),"E_remove_outside_pool_%u_keeps_live_renewal_until_release",last);record(name);
    }
    const unsigned addresses[]={101,50,100,200};
    for(size_t i=0;i<sizeof(addresses)/sizeof(*addresses);i++) {
        unsigned last=addresses[i];setup(true);assert(bind_owner(a,last,true)==ESP_OK);assert(commits==0);
        assert(ts_dhcp_server_save_config()==ESP_OK&&commits==1);
        setup(false);netifs_exist=false;assert(ts_dhcp_server_load_config()==ESP_OK);assert(eth()->ts_bindings.count==0);
        assert(s_state.iface[1].static_binding_count==1);netifs_exist=true;start_expected_owner=last;check_start_table=true;
        assert(ts_dhcp_server_start(1)==ESP_OK);assert(discover(a)==last);request(a,last);
        unsigned other=discover(b);assert(other>=100&&other<=103&&other!=last);
        char name[80];snprintf(name,sizeof(name),"F_saved_%u_load_before_netif_start_installed_before_first_offer",last);record(name);
    }
    /* F3: both interfaces must refuse inconsistent stored records and keep the
     * complete previous table, not just the count. */
    for(int index=0;index<TS_DHCP_IF_MAX;index++) {
        ts_dhcp_if_t iface=(ts_dhcp_if_t)index;
        const char *count_key=index?NVS_KEY_ETH_BIND_CNT:NVS_KEY_AP_BIND_CNT;
        const char *blob_key=index?NVS_KEY_ETH_BINDINGS:NVS_KEY_AP_BINDINGS;
        const char *tag=index?"eth":"ap";
        ts_dhcp_static_binding_t before[TS_DHCP_MAX_STATIC_BINDINGS];size_t before_count;char name[80];
        ts_dhcps_binding_table_t kept=servers[index].ts_bindings;

        setup(true);assert(bind_owner_if(iface,a,101,true)==ESP_OK);assert(ts_dhcp_server_save_config()==ESP_OK);
        assert(ts_dhcp_server_get_static_bindings(iface,before,TS_DHCP_MAX_STATIC_BINDINGS,&before_count)==ESP_OK);
        kept=servers[index].ts_bindings;
        assert(nvs_set_u8(0,count_key,(uint8_t)(TS_DHCP_MAX_STATIC_BINDINGS+1))==ESP_OK);
        assert(ts_dhcp_server_load_config()==ESP_ERR_INVALID_SIZE);
        assert(!memcmp(&kept,&servers[index].ts_bindings,sizeof(kept)));
        assert(s_state.iface[index].static_binding_count==before_count&&
               !memcmp(before,s_state.iface[index].static_bindings,before_count*sizeof(*before)));
        snprintf(name,sizeof(name),"F3_%s_count_above_capacity_rejected_current_owner_kept",tag);record(name);

        setup(true);assert(bind_owner_if(iface,a,101,true)==ESP_OK);assert(ts_dhcp_server_save_config()==ESP_OK);
        assert(ts_dhcp_server_get_static_bindings(iface,before,TS_DHCP_MAX_STATIC_BINDINGS,&before_count)==ESP_OK);
        kept=servers[index].ts_bindings;
        assert(shrink_blob(blob_key,1));
        assert(ts_dhcp_server_load_config()==ESP_ERR_INVALID_SIZE);
        assert(!memcmp(&kept,&servers[index].ts_bindings,sizeof(kept)));
        assert(s_state.iface[index].static_binding_count==before_count&&
               !memcmp(before,s_state.iface[index].static_bindings,before_count*sizeof(*before)));
        snprintf(name,sizeof(name),"F3_%s_short_blob_rejected_current_owner_kept",tag);record(name);

        setup(true);assert(bind_owner_if(iface,a,101,true)==ESP_OK);
        assert(bind_owner_if(iface,dmac,102,true)==ESP_OK);assert(ts_dhcp_server_save_config()==ESP_OK);
        assert(ts_dhcp_server_get_static_bindings(iface,before,TS_DHCP_MAX_STATIC_BINDINGS,&before_count)==ESP_OK&&before_count==2);
        kept=servers[index].ts_bindings;
        assert(nvs_set_u8(0,count_key,1)==ESP_OK);
        assert(ts_dhcp_server_load_config()==ESP_ERR_NVS_INVALID_LENGTH);
        assert(!memcmp(&kept,&servers[index].ts_bindings,sizeof(kept)));
        assert(s_state.iface[index].static_binding_count==before_count&&
               !memcmp(before,s_state.iface[index].static_bindings,before_count*sizeof(*before)));
        snprintf(name,sizeof(name),"F3_%s_overlong_blob_rejected_current_owner_kept",tag);record(name);
    }

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
