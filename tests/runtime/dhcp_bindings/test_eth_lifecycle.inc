/* Real manager creation/start/restart, ETH event/task and DHCP entry functions.
 * Hardware, event delivery and NVS are explicit host models. No device proof. */
#include "ts_eth.h"
#include "cJSON.h"
#include "ts_net.h"
#include "ts_wifi.h"
#define CONFIG_TS_NET_WIFI_ENABLE 1
#define CONFIG_TS_NET_WIFI_AP_SSID "test"
#define CONFIG_TS_NET_WIFI_AP_PASS ""
#define CONFIG_TS_NET_WIFI_AP_CHANNEL 1
#define CONFIG_TS_NET_WIFI_AP_MAX_CONN 4
#define WIFI_AUTH_OPEN 0
#define WIFI_AUTH_WPA2_PSK 3
#define WIFI_IF_STA 0
#define CONFIG_TS_NET_ETHERNET_ENABLE 1
#define ESP_NETIF_DHCP_SERVER 1
#define ESP_NETIF_NETSTACK_DEFAULT_ETH ((void*)1)
#define ETH_EVENT ((esp_event_base_t)3)
#define ETHERNET_EVENT_CONNECTED 1
#define ETHERNET_EVENT_DISCONNECTED 2
#define ETHERNET_EVENT_START 3
#define ETHERNET_EVENT_STOP 4
#define IP_EVENT_ETH_GOT_IP 2
#define IP_EVENT_ETH_LOST_IP 3
#define IP_EVENT_STA_GOT_IP 4
#define ETH_CMD_G_MAC_ADDR 1
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define portTICK_PERIOD_MS 1
typedef void *esp_eth_handle_t;
typedef struct {unsigned flags;const esp_netif_ip_info_t *ip_info;int get_ip_event,lost_ip_event;
    const char *if_key,*if_desc;int route_prio;} esp_netif_inherent_config_t;
typedef struct {const esp_netif_inherent_config_t *base;void *driver,*stack;} esp_netif_config_t;
static esp_netif_t *s_eth_netif;
static esp_eth_handle_t s_eth_handle=(void*)1;
static unsigned created_count, destroyed_count, driver_stops, driver_starts;
static bool driver_running, fail_driver_stop, partial_driver_stop, fail_driver_start;
static unsigned driver_stop_attempts;
static bool s_driver_stop_complete;
static void (*driver_stop_hook)(void);
static esp_netif_ip_info_t created_host;
static int event_queue[32];static size_t events_count;
static void (*delayed_tasks[16])(void*);static void *delayed_args[16];static unsigned delayed_count;
static pthread_mutex_t manager_mutex=PTHREAD_MUTEX_INITIALIZER;
static esp_err_t esp_netif_init(void){return ESP_OK;}
static esp_err_t esp_event_loop_create_default(void){return ESP_OK;}
static esp_err_t esp_netif_get_mac(esp_netif_t *n,uint8_t *mac){memset(mac,2,6);return ESP_OK;}
static esp_err_t esp_netif_get_dns_info(esp_netif_t *n,int kind,esp_netif_dns_info_t *dns){
    dns->ip.u_addr.ip4.addr=n->dhcps->dns_server[kind].addr;return ESP_OK;
}
static esp_err_t esp_netif_set_hostname(esp_netif_t *n,const char *name){return ESP_OK;}
static esp_err_t esp_netif_dhcpc_stop(esp_netif_t *n){return ESP_ERR_INVALID_ARG;}
static esp_err_t esp_netif_dhcpc_start(esp_netif_t *n){return ESP_ERR_INVALID_ARG;}
static void vTaskDelay(unsigned ticks){assert(!project_lock_depth&&!manager_lock_depth);}
static void vTaskDeleteWithCaps(void *task){}
static int xTaskCreateWithCaps(void (*fn)(void*),const char *name,unsigned stack,void *arg,
    unsigned priority,TaskHandle_t *task,unsigned caps){
    assert(delayed_count<16);delayed_tasks[delayed_count]=fn;delayed_args[delayed_count++]=arg;if(task)*task=(void*)1;return pdPASS;
}
static esp_err_t esp_eth_ioctl(esp_eth_handle_t h,int command,void *data){memset(data,2,6);return ESP_OK;}
static esp_err_t esp_eth_start(esp_eth_handle_t h){
    if(driver_running)return ESP_ERR_INVALID_STATE;
    if(fail_driver_start)return ESP_FAIL;
    driver_running=true;driver_starts++;
    event_queue[events_count++]=ETHERNET_EVENT_START;event_queue[events_count++]=ETHERNET_EVENT_CONNECTED;
    return ESP_OK;
}
static esp_err_t esp_eth_stop(esp_eth_handle_t h){
    driver_stop_attempts++;
    if(driver_stop_hook)driver_stop_hook();
    if(partial_driver_stop){driver_running=false;return ESP_ERR_INVALID_STATE;}
    if(fail_driver_stop)return ESP_FAIL;
    if(!driver_running)return ESP_ERR_INVALID_STATE;
    driver_running=false;driver_stops++;
    event_queue[events_count++]=ETHERNET_EVENT_DISCONNECTED;event_queue[events_count++]=ETHERNET_EVENT_STOP;
    return ESP_OK;
}
static esp_netif_t *esp_netif_new(const esp_netif_config_t *config){
    assert(config->base->flags==ESP_NETIF_DHCP_SERVER);
    created_host=*config->base->ip_info;created_count++;
    infos[1]=created_host;drain(&servers[1]);memset(&servers[1],0,sizeof(servers[1]));
    servers[1].state=DHCPS_HANDLE_CREATED;servers[1].dhcps_lease_time=DHCPS_LEASE_TIME_DEF;
    netifs[1]=(esp_netif_t){.dhcps=&servers[1],.ip_info=&infos[1],.status=ESP_NETIF_DHCP_INIT};
    eth_netif_exists=true;return &netifs[1];
}
static void esp_netif_destroy(esp_netif_t *netif){
    assert(netif==&netifs[1]&&!driver_running);destroyed_count++;
    drain(&servers[1]);eth_netif_exists=false;
}
#include "eth_creation.inc"
esp_err_t ts_eth_init(const ts_eth_config_t *config){
#include "eth_init_creation.inc"
    return s_eth_netif?ESP_OK:ESP_FAIL;
}
esp_netif_t *ts_eth_get_netif(void){return s_eth_netif;}
esp_err_t ts_eth_deinit(void){ts_eth_stop();s_eth_netif=NULL;eth_netif_exists=false;return ESP_OK;}
#include "eth_events.inc"
esp_err_t ts_net_mdns_start(void){return ESP_OK;}
static int manager_take(SemaphoreHandle_t mutex,int ticks){
    assert(!on_tcpip&&!project_lock_depth&&!manager_lock_depth);pthread_mutex_lock(mutex);manager_lock_depth++;return pdTRUE;
}
static void manager_give(SemaphoreHandle_t mutex){assert(manager_lock_depth==1);manager_lock_depth--;pthread_mutex_unlock(mutex);}
static SemaphoreHandle_t manager_create(void){return &manager_mutex;}
static esp_err_t load_eth_config_from_pins(ts_eth_config_t *config){*config=(ts_eth_config_t)TS_ETH_DEFAULT_CONFIG();return ESP_OK;}
static void manager_ip_event(void *arg,esp_event_base_t base,int32_t id,void *data){}
/* SDK leaves: configured mode is distinct from running/connected state. */
enum {WIFI_MODE_NULL,WIFI_MODE_STA,WIFI_MODE_AP,WIFI_MODE_APSTA};
typedef int wifi_mode_t;
#define ESP_ERR_WIFI_NOT_STARTED 0x3002
#define ESP_ERR_WIFI_NOT_INIT 0x3001
#define ESP_ERR_WIFI_CONN 0x3007
static wifi_mode_t wifi_sdk_mode;
static ts_wifi_mode_t s_mode;
static bool wifi_running;
static unsigned wifi_calls[5]; /* get_mode, disconnect, stop, set_mode, start */
static int wifi_failure=-1,scan_failure;
static bool scan_cleanup_failure;
static esp_err_t esp_wifi_get_mode(wifi_mode_t *mode){wifi_calls[0]++;if(wifi_failure==0)return ESP_ERR_WIFI_NOT_INIT;*mode=wifi_sdk_mode;return ESP_OK;}
static esp_err_t esp_wifi_disconnect(void){wifi_calls[1]++;return wifi_failure==1?ESP_FAIL:wifi_running?ESP_OK:ESP_ERR_WIFI_NOT_STARTED;}
static esp_err_t esp_wifi_stop(void){wifi_calls[2]++;if(wifi_failure==2)return ESP_ERR_WIFI_NOT_INIT;wifi_running=false;return ESP_OK;}
static esp_err_t esp_wifi_set_mode(wifi_mode_t mode){wifi_calls[3]++;if(wifi_failure==3)return ESP_FAIL;wifi_sdk_mode=mode;return ESP_OK;}
static esp_err_t esp_wifi_start(void){wifi_calls[4]++;if(wifi_failure==4)return ESP_FAIL;wifi_running=true;return ESP_OK;}
#include "wifi_core.inc"
esp_err_t ts_wifi_init(void){return ESP_OK;}
bool ts_wifi_sta_is_connected(void){return wifi_running&&(wifi_sdk_mode==WIFI_MODE_STA||wifi_sdk_mode==WIFI_MODE_APSTA);}
esp_err_t ts_wifi_sta_config(const ts_wifi_sta_config_t *c){return ESP_OK;}
esp_err_t ts_wifi_sta_connect(void){return esp_wifi_start();}
esp_err_t ts_wifi_ap_config(const ts_wifi_ap_config_t *c){return ESP_OK;}
#include "manager_types.inc"
static ts_net_manager_state_t manager_state;
#define s_state manager_state
#define xSemaphoreTake manager_take
#define xSemaphoreGive manager_give
#define xSemaphoreCreateMutex manager_create
#define ip_event_handler manager_ip_event
#include "manager_core.inc"
#undef ip_event_handler
#undef xSemaphoreCreateMutex
#undef xSemaphoreTake
#undef xSemaphoreGive
#undef s_state
#include "network_setter.inc"
const char *ts_net_if_to_str(ts_net_if_t iface){return iface==TS_NET_IF_ETH?"eth":"wifi";}
static ts_net_if_t net_parse_iface(const char *name){return !strcmp(name,"eth")?TS_NET_IF_ETH:TS_NET_IF_WIFI_STA;}
#define parse_iface net_parse_iface
#define ts_console_error ts_console_printf
#include "network_cli.inc"
#undef ts_console_error
#undef parse_iface
#include "status_api.inc"

/* Service-owner and registry boundaries; execute the real main hooks/API handlers. */
#include "ts_service.h"
#include "ts_time_sync.h"
static unsigned aux_starts;
esp_err_t ts_time_sync_init(const ts_time_sync_config_t *config){aux_starts++;return ESP_OK;}
static esp_err_t ts_device_lpmu_start_detection(void){aux_starts++;return ESP_OK;}
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#include "network_service.inc"
#define TS_EVENT_BASE_SERVICE 0
#include "service_types.inc"
static pthread_mutex_t registry_mutex=PTHREAD_MUTEX_INITIALIZER;
static ts_service_context_t s_svc_ctx;
static ts_service_instance_t network_instance;
static unsigned registry_events[4];
static void registry_take(SemaphoreHandle_t m,int ticks){assert(!pthread_mutex_lock(m));}
static void registry_give(SemaphoreHandle_t m){if(m)assert(!pthread_mutex_unlock(m));}
static int64_t esp_timer_get_time(void){return 0;}
static esp_err_t ts_event_post(int base,int event,const void *data,size_t size,unsigned ticks){registry_events[event]++;return ESP_OK;}
#define xSemaphoreTake registry_take
#define xSemaphoreGive registry_give
#include "service_core.inc"
#undef xSemaphoreGive
#undef xSemaphoreTake
static void reset_registry(void){
    network_instance=(ts_service_instance_t){.state=TS_SERVICE_STATE_RUNNING,.def={.name="network",.capabilities=TS_SERVICE_CAP_RESTARTABLE,.init=network_service_init,.start=network_service_start,.stop=network_service_stop}};
    s_svc_ctx=(ts_service_context_t){.initialized=true,.mutex=&registry_mutex,.services=&network_instance};
    memset(registry_events,0,sizeof(registry_events));
}
#define TS_API_ERR_BUSY 3
#define TS_API_ERR_INVALID_ARG 4
#define TS_API_ERR_NOT_FOUND 5
#include "service_api.inc"
#define TS_API_ERR_HARDWARE 6
static const char *wifi_mode_to_str(ts_wifi_mode_t m){return m==TS_WIFI_MODE_OFF?"off":m==TS_WIFI_MODE_AP?"ap":m==TS_WIFI_MODE_STA?"sta":"apsta";}
#include "wifi_api.inc"
typedef struct {struct {char ssid[32];} sta;} wifi_config_t;
static esp_err_t esp_wifi_set_config(int iface,const wifi_config_t *config){return ESP_OK;}
esp_err_t ts_wifi_scan_start(bool block){if(scan_cleanup_failure)wifi_failure=2;return scan_failure?ESP_FAIL:ESP_OK;}
esp_err_t ts_wifi_scan_get_results(ts_wifi_scan_result_t *results,uint16_t *count){*count=0;return ESP_OK;}
static const char *auth_mode_str(wifi_auth_mode_t mode){return "open";}
#define ts_console_error ts_console_printf
#include "wifi_cli.inc"
#undef ts_console_error

static void deliver_events(void){
    for(size_t i=0;i<events_count;i++){
        int event=event_queue[i];
        if(event==ETHERNET_EVENT_CONNECTED)netifs[1].up=true;
        if(event==ETHERNET_EVENT_DISCONNECTED||event==ETHERNET_EVENT_STOP)netifs[1].up=false;
        eth_event_handler(s_eth_netif,ETH_EVENT,event,&s_eth_handle);
        net_event_handler(NULL,ETH_EVENT,event,&s_eth_handle);
    }
    events_count=0;
}
static void run_delayed_task(void){
    for(unsigned i=0;i<delayed_count;i++)delayed_tasks[i](delayed_args[i]);delayed_count=0;
}
static void link_down(void){event_queue[events_count++]=ETHERNET_EVENT_DISCONNECTED;deliver_events();}
static void link_up(void){event_queue[events_count++]=ETHERNET_EVENT_CONNECTED;deliver_events();}
static void seed_network(const char *host,const char *gateway,unsigned mode){
    nvs_set_u8(2,"eth_en",1);nvs_set_u8(2,"eth_ipmode",mode);
    nvs_set_str(2,"eth_ip",host);nvs_set_str(2,"eth_mask","255.255.255.0");
    nvs_set_str(2,"eth_gw",gateway);nvs_set_str(2,"eth_dns1","8.8.8.8");
}
static void seed_dhcp_eth(const char *prefix){
    nvs_set_u8(0,NVS_KEY_ETH_ENABLED,1);char value[16];
    const unsigned last[]={100,103,254};const unsigned fields[]={1,2,3};
    for(unsigned i=0;i<3;i++){snprintf(value,sizeof(value),"%s.%u",prefix,last[i]);set_saved_string(1,fields[i],value);}
    set_saved_string(1,4,"255.255.255.0");set_saved_string(1,5,"8.8.8.8");nvs_set_u32(0,NVS_KEY_ETH_LEASE,120);
    ts_dhcp_static_binding_t owner={.enabled=true};memcpy(owner.mac,a,6);snprintf(owner.ip,sizeof(owner.ip),"%s.50",prefix);
    nvs_set_blob(0,NVS_KEY_ETH_BINDINGS,&owner,sizeof(owner));nvs_set_u8(0,NVS_KEY_ETH_BIND_CNT,1);
}
static void hardware_reset(void){
    boot_defaults(false);memset(&manager_state,0,sizeof(manager_state));
    s_state.initialized=false;s_state.mutex=NULL;eth_netif_exists=false;s_eth_netif=NULL;s_link_up=false;
    created_count=destroyed_count=driver_starts=driver_stops=0;events_count=0;driver_running=false;
    driver_stop_hook=NULL;fail_driver_stop=partial_driver_stop=fail_driver_start=false;driver_stop_attempts=0;
    s_driver_stop_complete=false;wifi_present=true;wifi_running=false;wifi_sdk_mode=WIFI_MODE_STA;s_mode=TS_WIFI_MODE_OFF;wifi_failure=-1;scan_failure=0;scan_cleanup_failure=false;memset(wifi_calls,0,sizeof(wifi_calls));reset_registry();
    delayed_count=0;started_notifications=stopped_notifications=0;
}
/* Shared by ordinary boot acceptance and the optional historical probes. */
static int boot_eth(bool corrupt){
    setup(true);
    const char *prefix=corrupt?"10.10.99":"192.168.50";
    seed_network(corrupt?"10.10.99.97":"192.168.50.1",corrupt?"10.10.99.100":"192.168.50.254",TS_NET_IP_MODE_STATIC);
    seed_dhcp_eth(prefix);if(corrupt)assert(shrink_blob(NVS_KEY_ETH_BINDINGS,1));
    hardware_reset();assert(ts_net_manager_init()==ESP_OK);
    esp_err_t restored=ts_dhcp_server_init(),started=ts_net_manager_start(TS_NET_IF_ETH);
    deliver_events();run_delayed_task();
    if(corrupt)return started!=ESP_OK||!driver_running?15:
        restored==ESP_OK||netifs[1].status==ESP_NETIF_DHCP_STARTED||ts_dhcp_server_get_restore_error(1)==ESP_OK?16:0;
    if(created_host.ip.addr!=inet_addr("192.168.50.1"))return 12;
    if(restored!=ESP_OK||started!=ESP_OK||netifs[1].status!=ESP_NETIF_DHCP_STARTED||servers[1].ts_bindings.count!=1)return 13;
    return packet(&servers[1],a,DHCPDISCOVER,0,0,DHCPS_STATE_OFFER)==50?0:14;
}
static void prepare_custom_boot(void){assert(boot_eth(false)==0);}
static ts_net_if_config_t network_candidate(const char *host,const char *gateway){
    ts_net_if_config_t config;assert(ts_net_manager_get_config(TS_NET_IF_ETH,&config)==ESP_OK);
    snprintf(config.static_ip.ip,sizeof(config.static_ip.ip),"%s",host);
    snprintf(config.static_ip.gateway,sizeof(config.static_ip.gateway),"%s",gateway);return config;
}
static void prepare_pool(const char *prefix){
    ts_dhcp_config_t config;assert(ts_dhcp_server_get_config(1,&config)==ESP_OK);
    snprintf(config.pool.start_ip,sizeof(config.pool.start_ip),"%s.100",prefix);
    snprintf(config.pool.end_ip,sizeof(config.pool.end_ip),"%s.103",prefix);
    snprintf(config.pool.gateway,sizeof(config.pool.gateway),"%s.254",prefix);
    assert(ts_dhcp_server_set_config(1,&config)==ESP_OK);
}
static void check_eth_lifecycle(void){
    for(unsigned custom=0;custom<2;custom++){
    setup(true);seed_network("192.168.50.1","192.168.50.254",TS_NET_IP_MODE_STATIC);
    nvs_set_str(2,"eth_dns1","9.9.9.9");seed_dhcp_eth("192.168.50");if(!custom)set_saved_string(1,5,"");
    hardware_reset();assert(ts_net_manager_init()==ESP_OK&&ts_dhcp_server_init()==ESP_OK);
    assert(ts_net_manager_start(TS_NET_IF_ETH)==ESP_OK);deliver_events();run_delayed_task();
    ts_net_ip_info_t cold_dns={0};assert(ts_net_get_ip_info(TS_NET_IF_ETH,&cold_dns)==ESP_OK);
    uint32_t expected=inet_addr(custom?"8.8.8.8":"9.9.9.9");
    assert(cold_dns.dns1==expected&&dns_writes[0]==(custom?2:1));
    cJSON *visible=cJSON_CreateObject();add_ip_info_to_json(visible,&cold_dns);
    assert(!strcmp(cJSON_GetObjectItem(visible,"dns1")->valuestring,custom?"8.8.8.8":"9.9.9.9"));cJSON_Delete(visible);
    assert(option_value(&servers[1],DHCP_OPTION_DNS_SERVER,true)==inet_addr(custom?"8.8.8.8":"192.168.50.1"));
    assert(ts_dhcp_server_load_config()==ESP_OK);
    assert(ts_net_get_ip_info(TS_NET_IF_ETH,&cold_dns)==ESP_OK&&cold_dns.dns1==expected);
    }
    record("P2_cold_boot_same_host_DNS_readback_pool_override_empty_offer_and_reload");
    for(unsigned kind=0;kind<2;kind++){
        setup(true);hardware_reset();size_t nvs_size=kv_count;unsigned char stored[sizeof(kv)];memcpy(stored,kv,sizeof(kv));
        ts_eth_config_t config=TS_ETH_DEFAULT_CONFIG();config.ip_info=infos[1];
        config.dns[0]=inet_addr("9.9.9.9");config.dns[1]=inet_addr("1.1.1.1");dns_failure_kind=kind;
        assert(ts_eth_init(&config)==ESP_ERR_INVALID_STATE);
        assert(!s_eth_netif&&!eth_netif_exists&&created_count==1&&destroyed_count==1&&!driver_starts);
        assert(kv_count==nvs_size&&!memcmp(stored,kv,sizeof(kv)));
    }
    setup(true);seed_network("192.168.50.1","192.168.50.254",TS_NET_IP_MODE_STATIC);hardware_reset();dns_failure_kind=0;
    assert(ts_net_manager_init()==ESP_OK);ts_net_if_status_t failed;
    assert(ts_net_manager_get_if_status(TS_NET_IF_ETH,&failed)==ESP_OK&&failed.state==TS_NET_STATE_ERROR);
    assert(ts_net_manager_start(TS_NET_IF_ETH)==ESP_ERR_NOT_FOUND&&!driver_starts&&destroyed_count==1);
    record("P2_DNS_main_backup_failure_propagates_cleans_netif_and_manager_does_not_start");
    assert(boot_eth(true)==0);
    ts_net_ip_info_t host_dns={0};assert(ts_net_get_ip_info(TS_NET_IF_ETH,&host_dns)==ESP_OK&&host_dns.dns1==inet_addr("8.8.8.8"));
    assert(driver_running&&s_link_up&&netifs[1].status!=ESP_NETIF_DHCP_STARTED);
    ts_dhcp_status_t blocked;assert(ts_dhcp_server_get_status(1,&blocked)==ESP_OK&&blocked.restore_error!=ESP_OK);
    assert(servers[1].ts_bindings.count==0&&ts_dhcp_server_save_config()!=ESP_OK);
    record("ETH_valid_host_starts_with_corrupt_DHCP_record_DHCP_stays_blocked_and_protected");
    setup(true);seed_network("192.168.50.1","192.168.50.254",TS_NET_IP_MODE_STATIC);hardware_reset();
    assert(ts_net_manager_init()==ESP_OK&&ts_dhcp_server_init()==ESP_OK);
    assert(ts_net_manager_start(TS_NET_IF_ETH)==ESP_OK);deliver_events();run_delayed_task();
    assert(driver_running&&s_link_up&&netifs[1].status!=ESP_NETIF_DHCP_STARTED);
    assert(ts_dhcp_server_get_status(1,&blocked)==ESP_OK&&blocked.apply_error==ESP_ERR_INVALID_ARG);
    link_down();unsigned before=apply_dispatches;link_up();run_delayed_task();assert(apply_dispatches==before);
    record("ETH_valid_custom_host_without_matching_DHCP_pool_is_reachable_failed_resume_not_retried");
    prepare_custom_boot();
    assert(servers[1].server_address.addr==inet_addr("192.168.50.1"));
    assert(option_value(&servers[1],51,true)==htonl(7200));
    assert(created_host.ip.addr!=inet_addr("192.168.50.254")); /* Pool gateway is not host IP. */
    record("ETH_P1_real_manager_create_restore_start_first_OFFER_custom_subnet");

    link_down();assert(eth_run_requested()&&netifs[1].status==ESP_NETIF_DHCP_STOPPED);
    assert(ts_dhcp_server_load_config()==ESP_OK);assert(eth_run_requested());
    link_up();run_delayed_task();assert(netifs[1].status==ESP_NETIF_DHCP_STARTED);
    record("ETH_disconnect_reload_reconnect_preserves_run_request");
    link_down();assert(ts_dhcp_server_stop(1)==ESP_OK);link_up();unsigned notices=started_notifications;
    run_delayed_task();assert(!eth_run_requested()&&netifs[1].status==ESP_NETIF_DHCP_STOPPED);
    assert(started_notifications==notices);
    record("ETH_user_stop_while_disconnected_wins_over_reconnect");
    assert(ts_dhcp_server_start(1)==ESP_OK);link_down();link_up();
    assert(delayed_count);assert(ts_dhcp_server_stop(1)==ESP_OK);notices=started_notifications;
    run_delayed_task();assert(!eth_run_requested()&&netifs[1].status==ESP_NETIF_DHCP_STOPPED&&started_notifications==notices);
    record("ETH_CONNECTED_queued_then_user_stop_wins_over_delayed_resume");

    prepare_custom_boot();link_down();link_up();link_down();link_up();
    assert(delayed_count==2);notices=started_notifications;run_delayed_task();
    assert(netifs[1].status==ESP_NETIF_DHCP_STARTED&&started_notifications==notices+1);
    link_down();link_up();link_down();link_up();assert(ts_dhcp_server_stop(1)==ESP_OK);
    notices=started_notifications;run_delayed_task();
    assert(netifs[1].status==ESP_NETIF_DHCP_STOPPED&&started_notifications==notices);
    record("ETH_multiple_queued_CONNECT_events_use_current_request_and_notify_once");

    prepare_custom_boot();ts_net_if_config_t candidate=network_candidate("192.168.60.1","192.168.60.254");
    assert(ts_net_manager_set_config(TS_NET_IF_ETH,&candidate)==ESP_OK);
    unsigned stops=driver_stops, sdk_stops=stop_calls;ts_dhcps_binding_table_t old=servers[1].ts_bindings;
    assert(ts_net_manager_restart(TS_NET_IF_ETH)==ESP_ERR_INVALID_ARG);
    assert(driver_stops==stops&&stop_calls==sdk_stops&&eth_run_requested()&&netifs[1].status==ESP_NETIF_DHCP_STARTED);
    assert(!memcmp(&old,&servers[1].ts_bindings,sizeof(old))&&infos[1].ip.addr==inet_addr("192.168.50.1"));
    record("ETH_net_restart_invalid_candidate_preserves_interface_DHCP_request_and_table");
    /* Bindings are explicit edits on the current subnet; clearing is intentional. */
    assert(ts_dhcp_server_clear_static_bindings(1)==ESP_OK);prepare_pool("192.168.60");
    assert(ts_net_manager_restart(TS_NET_IF_ETH)==ESP_OK);deliver_events();run_delayed_task();
    assert(driver_stops==stops+1&&eth_run_requested()&&netifs[1].status==ESP_NETIF_DHCP_STARTED);
    assert(infos[1].ip.addr==inet_addr("192.168.60.1"));
    assert(packet(&servers[1],b,DHCPDISCOVER,0,0,DHCPS_STATE_OFFER)==100);
    record("ETH_net_restart_valid_complete_candidate_new_subnet_first_offer");
    assert(ts_dhcp_server_stop(1)==ESP_OK);
    assert(ts_net_manager_restart(TS_NET_IF_ETH)==ESP_OK);deliver_events();notices=started_notifications;run_delayed_task();
    assert(!eth_run_requested()&&netifs[1].status==ESP_NETIF_DHCP_STOPPED&&started_notifications==notices);
    record("ETH_net_restart_complete_event_chain_preserves_explicit_stop");

    prepare_custom_boot();assert(ts_dhcp_server_clear_static_bindings(1)==ESP_OK);prepare_pool("192.168.60");
    ts_net_ip_info_t host={.ip=inet_addr("192.168.60.1"),.netmask=inet_addr("255.255.255.0"),.gateway=inet_addr("192.168.60.254"),.dns1=inet_addr("8.8.8.8")};
    assert(ts_net_set_ip_info(TS_NET_IF_ETH,&host)==ESP_OK);
    assert(netifs[1].status==ESP_NETIF_DHCP_STARTED&&infos[1].ip.addr==host.ip);
    ts_net_if_status_t actual;assert(ts_net_manager_get_if_status(TS_NET_IF_ETH,&actual)==ESP_OK);
    assert(!strcmp(actual.ip_info.ip,"192.168.60.1"));
    record("ETH_public_low_level_setter_uses_same_transaction_and_reports_actual_address");

    for(unsigned which=0;which<9;which++){
        setup(true);seed_network(which==0?"bad":which==1?"":"192.168.50.1","192.168.50.254",which==2?TS_NET_IP_MODE_DHCP:TS_NET_IP_MODE_STATIC);
        if(which==4)nvs_set_str(2,"eth_mask","255.0.255.0");
        if(which==5)nvs_set_str(2,"eth_ip","192.168.50.0");
        if(which==6)nvs_set_str(2,"eth_ip","192.168.50.255");
        if(which==7)nvs_set_str(2,"eth_gw","192.168.60.254");
        if(which==8)nvs_set_str(2,"eth_ip","192.168.50.100-extra");
        if(which==3)nvs_set_u8(2,"eth_mask",7); /* Wrong NVS type must not become a default. */
        size_t records=kv_count;unsigned char stored[sizeof(kv)];memcpy(stored,kv,sizeof(kv));hardware_reset();assert(ts_net_manager_init()==ESP_OK);
        assert(!created_count&&!s_eth_netif&&manager_state.eth_status.state==TS_NET_STATE_ERROR);
        assert(manager_state.eth_status.config_error!=ESP_OK&&kv_count==records);
        assert(ts_dhcp_server_init()==ESP_OK);ts_dhcp_status_t status;
        assert(ts_dhcp_server_get_status(1,&status)==ESP_OK);
        assert(!status.interface_present&&status.state==TS_DHCP_STATE_STOPPED&&status.resume_error==ESP_ERR_NOT_FOUND);
        assert(ts_net_manager_start(TS_NET_IF_ETH)!=ESP_OK);
        cJSON *data=cJSON_CreateObject();add_recovery_status(data,1,&status);add_eth_config_status(data);
        assert(cJSON_GetObjectItem(data,"config_error")->valueint==manager_state.eth_status.config_error);
        assert(cJSON_IsFalse(cJSON_GetObjectItem(data,"interface_present")));
        assert(cJSON_GetObjectItem(data,"resume_error")->valueint==ESP_ERR_NOT_FOUND);cJSON_Delete(data);
        assert(ts_net_get_status(TS_NET_IF_ETH)==TS_NET_STATUS_ERROR);
        console_text[0]=0;assert(do_net_ip("eth",true)==0);data=cJSON_Parse(console_text);
        assert(data&&cJSON_GetObjectItem(data,"config_error")->valueint==manager_state.eth_status.config_error);cJSON_Delete(data);
        assert(ts_net_manager_save_config()!=ESP_OK&&kv_count==records&&!memcmp(stored,kv,sizeof(kv)));
        assert(ts_dhcp_server_eth_resume()==ESP_ERR_NOT_FOUND);
    }
    record("ETH_invalid_empty_unsupported_and_NVS_type_error_no_creation_visible_preserved");
    prepare_custom_boot();candidate=network_candidate("192.168.50.1","192.168.50.254");
    assert(ts_net_manager_set_ip_mode(TS_NET_IF_ETH,TS_NET_IP_MODE_DHCP)==ESP_ERR_NOT_SUPPORTED);
    assert(do_net_set("eth",NULL,NULL,NULL,NULL,"dhcp",NULL)==1);
    ts_net_if_config_t retained;assert(ts_net_manager_get_config(TS_NET_IF_ETH,&retained)==ESP_OK);
    assert(retained.ip_mode==TS_NET_IP_MODE_STATIC);
    record("ETH_client_DHCP_mode_rejected_API_and_actual_CLI_without_false_success");

    fail_stage=FAIL_BINDINGS;assert(ts_dhcp_server_restart(1)==ESP_FAIL);
    link_down();link_up();notices=started_notifications;run_delayed_task();
    ts_dhcp_status_t status;assert(ts_dhcp_server_get_status(1,&status)==ESP_OK);
    assert(status.want_running&&status.apply_error==ESP_FAIL&&status.resume_error==ESP_FAIL&&started_notifications==notices);
    assert(netifs[1].status==ESP_NETIF_DHCP_STOPPED);
    record("ETH_resume_blocks_failed_application_without_retry_and_reports_reason");
    prepare_custom_boot();prepare_pool("192.168.60");
    assert(ts_dhcp_server_set_lease_time(1,999)==ESP_OK);
    link_down();link_up();run_delayed_task();
    assert(netifs[1].status==ESP_NETIF_DHCP_STARTED&&servers[1].dhcps_lease_time==120);
    assert(servers[1].dhcps_poll.start_ip.addr==inet_addr("192.168.50.100"));
    assert(!strcmp(s_state.iface[1].config.pool.start_ip,"192.168.60.100")&&s_state.iface[1].config.lease_time_min==999);
    record("ETH_link_resume_uses_applied_config_and_preserves_pending_edits");
    prepare_custom_boot();netifs[1].up=false;
    unsigned no_dispatch=apply_dispatches;assert(ts_dhcp_server_eth_resume()==ESP_ERR_ESP_NETIF_IF_NOT_READY);
    assert(apply_dispatches==no_dispatch&&ts_dhcp_server_get_status(1,&status)==ESP_OK);
    assert(status.link_up&&status.want_running&&status.resume_error==ESP_ERR_ESP_NETIF_IF_NOT_READY);
    record("ETH_resume_distinguishes_netif_not_ready_from_user_stop_or_physical_link_down");
    prepare_custom_boot();link_down();
    s_state.iface[1].config_restore_error=ESP_ERR_NVS_TYPE_MISMATCH;
    link_up();unsigned dispatches=apply_dispatches;run_delayed_task();
    assert(apply_dispatches==dispatches&&eth_run_requested());
    assert(ts_dhcp_server_get_status(1,&status)==ESP_OK&&status.resume_error==ESP_ERR_NVS_TYPE_MISMATCH);
    record("ETH_resume_blocks_storage_error_without_dispatch_and_reports_actual_reason");
    prepare_custom_boot();assert(ts_dhcp_server_stop(1)==ESP_OK);assert(ts_dhcp_server_load_config()==ESP_OK);
    assert(!eth_run_requested()&&netifs[1].status==ESP_NETIF_DHCP_STOPPED);
    notices=started_notifications;assert(ts_dhcp_server_eth_resume()==ESP_ERR_INVALID_STATE);
    assert(started_notifications==notices);
    record("ETH_explicit_stop_then_reload_does_not_rewrite_request_or_notify_start");
    boot_defaults(true);interface_up(0);unsigned stopped=stopped_notifications;
    assert(ts_dhcp_server_stop(0)==ESP_OK&&stopped_notifications==stopped+1);
    record("AP_auto_started_stop_notification_uses_real_SDK_state_outside_lock");
    setup(true); /* Release model leases and restore original test environment. */
}
/* Frozen P2 boundaries: real callback registration, caller reentry, two callers,
 * driver-vs-DHCP failure, and rejected/accepted intent. No device acceptance. */
static unsigned control_callbacks;
static bool callback_start_dhcp;
static void control_callback(ts_dhcp_if_t iface,ts_dhcp_event_t event,const ts_dhcp_client_t *client,void *arg){
    if(iface!=TS_DHCP_IF_ETH||event!=TS_DHCP_EVENT_STOPPED)return;
    control_callbacks++;
    ts_net_manager_status_t status;assert(ts_net_manager_get_status(&status)==ESP_OK);
    assert(!status.eth.link_up&&!status.eth.has_ip&&status.eth.state!=TS_NET_STATE_GOT_IP);
    (void)ts_net_get_status(TS_NET_IF_ETH);
    assert(ts_net_manager_start(TS_NET_IF_ETH)==ESP_ERR_NOT_FINISHED);
    assert(ts_net_manager_stop(TS_NET_IF_ETH)==ESP_ERR_NOT_FINISHED);
    assert(ts_net_manager_restart(TS_NET_IF_ETH)==ESP_ERR_NOT_FINISHED);
    assert(ts_net_manager_deinit()==ESP_ERR_NOT_FINISHED);
    int (*commands[])(const char*)={do_net_start,do_net_stop,do_net_restart};
    for(unsigned i=0;i<3;i++){console_text[0]=0;assert(commands[i]("eth")==1&&strstr(console_text,"operation in progress"));}
    cJSON *params=cJSON_CreateObject();cJSON_AddStringToObject(params,"name","network");
    esp_err_t (*apis[])(const cJSON*,ts_api_result_t*)={api_service_start,api_service_stop,api_service_restart};
    for(unsigned i=0;i<3;i++){
        network_instance.state=i?TS_SERVICE_STATE_RUNNING:TS_SERVICE_STATE_STOPPED;
        ts_api_result_t result={0};unsigned auxiliary=aux_starts;
        assert(apis[i](params,&result)==ESP_ERR_NOT_FINISHED&&result.code==TS_API_ERR_BUSY);
        assert(strstr(result.message,"Operation in progress")&&aux_starts==auxiliary);
        ts_api_result_free(&result);
    }
    network_instance.state=TS_SERVICE_STATE_RUNNING;
    cJSON_Delete(params);
    if(callback_start_dhcp){
        unsigned starts=start_calls;
        assert(ts_dhcp_server_start(TS_DHCP_IF_ETH)==ESP_OK);
        assert(!s_link_up&&netifs[1].status==ESP_NETIF_DHCP_STOPPED&&start_calls==starts);
    }
}
static pthread_mutex_t control_barrier=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t control_condition=PTHREAD_COND_INITIALIZER;
static bool driver_entered,driver_continue;
static void pause_driver_stop(void){
    pthread_mutex_lock(&control_barrier);driver_entered=true;pthread_cond_broadcast(&control_condition);
    while(!driver_continue)pthread_cond_wait(&control_condition,&control_barrier);
    pthread_mutex_unlock(&control_barrier);
}
static void *stop_network_thread(void *arg){assert(ts_net_manager_stop(TS_NET_IF_ETH)==ESP_OK);return NULL;}
static void check_manager_control_boundaries(void){
    for(unsigned teardown=0;teardown<2;teardown++){
        prepare_custom_boot();control_callbacks=0;callback_start_dhcp=!teardown;
        assert(ts_dhcp_server_register_event_cb(control_callback,NULL)==ESP_OK);
        assert((teardown?ts_net_manager_deinit():ts_net_manager_stop(TS_NET_IF_ETH))==ESP_OK);
        assert(control_callbacks==1&&netifs[1].status==ESP_NETIF_DHCP_STOPPED);
        assert(ts_dhcp_server_unregister_event_cb(control_callback)==ESP_OK);
        if(!teardown){assert(eth_run_requested());assert(ts_net_manager_start(TS_NET_IF_ETH)==ESP_OK);deliver_events();run_delayed_task();assert(netifs[1].status==ESP_NETIF_DHCP_STARTED);}
        else {assert(!ts_net_manager_is_initialized());assert(ts_net_manager_start(TS_NET_IF_ETH)==ESP_ERR_INVALID_STATE);}
    }
    record("P2_manager_stop_deinit_callback_queries_succeed_reentry_busy_DHCP_start_defers_until_link");

    prepare_custom_boot();driver_entered=driver_continue=false;driver_stop_hook=pause_driver_stop;
    pthread_t thread;assert(!pthread_create(&thread,NULL,stop_network_thread,NULL));
    pthread_mutex_lock(&control_barrier);while(!driver_entered)pthread_cond_wait(&control_condition,&control_barrier);pthread_mutex_unlock(&control_barrier);
    assert(ts_net_manager_start(TS_NET_IF_ETH)==ESP_ERR_NOT_FINISHED);
    assert(ts_net_manager_stop(TS_NET_IF_ETH)==ESP_ERR_NOT_FINISHED);
    assert(ts_net_manager_restart(TS_NET_IF_ETH)==ESP_ERR_NOT_FINISHED);
    assert(ts_net_manager_deinit()==ESP_ERR_NOT_FINISHED);
    ts_net_manager_status_t status;assert(ts_net_manager_get_status(&status)==ESP_OK);
    assert(status.eth.link_up&&driver_running); /* Operation request is not a stopped fact. */
    pthread_mutex_lock(&control_barrier);driver_continue=true;pthread_cond_broadcast(&control_condition);pthread_mutex_unlock(&control_barrier);
    assert(!pthread_join(thread,NULL));driver_stop_hook=NULL;
    assert(ts_net_manager_get_status(&status)==ESP_OK&&status.eth.state==TS_NET_STATE_INITIALIZED&&!status.eth.link_up);
    assert(ts_net_manager_start(TS_NET_IF_ETH)==ESP_OK);deliver_events();run_delayed_task();
    record("P2_two_threads_ETH_operation_busy_queries_available_guard_released_after_stop");

    for(unsigned teardown=0;teardown<2;teardown++)for(unsigned driver_failure=0;driver_failure<2;driver_failure++){
        prepare_custom_boot();if(driver_failure)fail_driver_stop=true;else fail_stage=FAIL_STOP;
        assert((teardown?ts_net_manager_deinit():ts_net_manager_stop(TS_NET_IF_ETH))==ESP_FAIL);
        assert(ts_net_manager_is_initialized()&&ts_eth_get_netif());
        ts_dhcp_status_t dhcp;assert(ts_net_manager_get_status(&status)==ESP_OK&&status.eth.state==TS_NET_STATE_ERROR);
        assert(ts_dhcp_server_get_status(TS_DHCP_IF_ETH,&dhcp)==ESP_OK);
        assert(status.eth.link_up==(bool)driver_failure&&status.eth.has_ip==(bool)driver_failure);
        assert(dhcp.apply_error==(driver_failure?ESP_OK:ESP_FAIL)&&dhcp.state==TS_DHCP_STATE_RUNNING);
        fail_driver_stop=false;
        if(!driver_failure){deliver_events();assert(ts_net_manager_start(TS_NET_IF_ETH)==ESP_OK);deliver_events();run_delayed_task();assert(netifs[1].status==ESP_NETIF_DHCP_STOPPED&&ts_dhcp_server_get_apply_error(1)==ESP_FAIL);}
    }
    record("P2_driver_and_DHCP_stop_failures_keep_actual_state_and_correct_error_owner");

}
static void check_start_intent_boundaries(void){
    for(unsigned restart=0;restart<2;restart++)for(unsigned rejection=0;rejection<3;rejection++){
        prepare_custom_boot();assert(ts_dhcp_server_stop(1)==ESP_OK);
        if(rejection==0)prepare_pool("192.168.60");
        if(rejection==1){fail_open_read=true;assert(ts_dhcp_server_load_config()==ESP_FAIL);fail_open_read=false;}
        if(rejection==2)fail_stage=FAIL_STATUS;
        esp_err_t ret=restart?ts_dhcp_server_restart(1):ts_dhcp_server_start(1);
        assert(ret==(rejection==0?ESP_ERR_INVALID_ARG:ESP_FAIL));
        assert(!eth_run_requested()&&ts_dhcp_server_get_apply_error(1)==ESP_OK);
        unsigned starts=started_notifications;link_down();link_up();run_delayed_task();
        assert(netifs[1].status==ESP_NETIF_DHCP_STOPPED&&!eth_run_requested()&&started_notifications==starts);
    }
    record("P2_rejected_start_restart_from_stopped_preserve_intent_across_reconnect");

    const enum failure_stage stages[]={FAIL_STOP,FAIL_BINDINGS,FAIL_START};
    for(unsigned requested=0;requested<2;requested++)for(unsigned i=0;i<3;i++){
        prepare_custom_boot();if(!requested)assert(ts_dhcp_server_stop(1)==ESP_OK);
        assert(ts_dhcp_server_set_lease_time(1,121)==ESP_OK);fail_stage=stages[i];
        assert(ts_dhcp_server_start(1)==ESP_FAIL&&eth_run_requested());
        assert(ts_dhcp_server_get_apply_error(1)==ESP_FAIL);
        link_down();unsigned dispatches=apply_dispatches,starts=started_notifications;link_up();run_delayed_task();
        assert(netifs[1].status==ESP_NETIF_DHCP_STOPPED&&apply_dispatches==dispatches&&started_notifications==starts);
    }
    record("P2_accepted_start_SDK_failure_including_stop_latches_error_and_blocks_resume");
    setup(true);
}

/* Same service-registry path for both historical P1 counterexamples. */
static int service_restart_probe(bool already_stopped){
    prepare_custom_boot();
    if(already_stopped){assert(ts_net_manager_stop(TS_NET_IF_ETH)==ESP_OK);deliver_events();}
    unsigned starts=driver_starts;
    esp_err_t ret=ts_service_restart(&network_instance);
    deliver_events();run_delayed_task();
    return ret!=ESP_OK||network_instance.state!=TS_SERVICE_STATE_RUNNING||!driver_running||driver_starts!=starts+1||netifs[1].status!=ESP_NETIF_DHCP_STARTED||
        registry_events[TS_EVENT_SERVICE_STARTED]!=1||registry_events[TS_EVENT_SERVICE_STOPPED]!=1||
        packet(&servers[1],a,DHCPDISCOVER,0,0,DHCPS_STATE_OFFER)!=50 ? (already_stopped?17:18):0;
}
static void check_network_stop_contract(void){
    assert(service_restart_probe(true)==0);assert(service_restart_probe(false)==0);
    record("P1_real_registry_restart_after_ETH_stop_and_with_unstarted_STA");

    prepare_custom_boot();manager_state.eth_config.enabled=false;wifi_present=false;
    assert(ts_service_stop(&network_instance)==ESP_OK&&!driver_running);
    unsigned attempts=driver_stop_attempts,stops=stop_calls;
    assert(ts_net_manager_stop(TS_NET_IF_ETH)==ESP_OK&&driver_stop_attempts==attempts&&stop_calls==stops+1);
    assert(ts_net_manager_deinit()==ESP_OK&&driver_stop_attempts==attempts);
    prepare_custom_boot();link_down();assert(driver_running);
    attempts=driver_stop_attempts;assert(ts_net_manager_stop(TS_NET_IF_ETH)==ESP_OK&&driver_stop_attempts==attempts+1);
    hardware_reset();assert(ts_net_manager_init()==ESP_OK&&ts_dhcp_server_init()==ESP_OK);
    assert(ts_net_manager_stop(TS_NET_IF_ETH)==ESP_OK&&driver_stop_attempts==0);
    s_eth_netif=NULL;eth_netif_exists=false;wifi_present=false;
    assert(network_service_stop(NULL,NULL)==ESP_OK);
    record("ETH_confirmed_stop_repeats_suspend_deinit_and_link_down_still_stops_driver_absent_interfaces_skip");

    for(unsigned failure=0;failure<2;failure++){
        prepare_custom_boot();unsigned auxiliary=aux_starts;
        if(failure){assert(ts_net_manager_stop(TS_NET_IF_ETH)==ESP_OK);fail_driver_start=true;assert(ts_net_manager_start(TS_NET_IF_ETH)==ESP_FAIL);fail_driver_start=false;}
        else partial_driver_stop=true;
        assert(ts_service_restart(&network_instance)==ESP_ERR_INVALID_STATE);
        partial_driver_stop=false;attempts=driver_stop_attempts;
        assert(ts_service_restart(&network_instance)==ESP_ERR_INVALID_STATE&&driver_stop_attempts==attempts+1);
        assert(!driver_running&&network_instance.state==TS_SERVICE_STATE_RUNNING&&aux_starts==auxiliary);
    }
    prepare_custom_boot();fail_stage=FAIL_STOP;
    assert(ts_service_restart(&network_instance)==ESP_FAIL&&!driver_running);
    attempts=driver_stop_attempts;stops=stop_calls;fail_stage=FAIL_STOP;
    assert(ts_service_restart(&network_instance)==ESP_FAIL&&driver_stop_attempts==attempts&&stop_calls==stops+1);
    assert(ts_service_stop(&network_instance)==ESP_OK&&driver_stop_attempts==attempts&&netifs[1].status==ESP_NETIF_DHCP_STOPPED);
    assert(ts_dhcp_server_get_apply_error(1)==ESP_FAIL); /* No automatic error clearing. */
    record("ETH_uncertain_SDK_failures_block_registry_restart_confirmed_driver_stop_never_skips_DHCP_failure");

    for(unsigned mode=0;mode<4;mode++){
        prepare_custom_boot();wifi_sdk_mode=mode;wifi_running=mode==WIFI_MODE_AP;
        assert(ts_net_manager_stop(TS_NET_IF_WIFI_STA)==ESP_OK);
        assert(wifi_calls[1]==(mode==WIFI_MODE_STA||mode==WIFI_MODE_APSTA));
        assert(!wifi_calls[2]&&!wifi_calls[3]&&!wifi_calls[4]);
        assert(wifi_running==(mode==WIFI_MODE_AP)&&wifi_sdk_mode==(int)mode);
    }
    for(unsigned ap=0;ap<2;ap++){
        prepare_custom_boot();wifi_sdk_mode=ap?WIFI_MODE_APSTA:WIFI_MODE_STA;wifi_running=true;s_mode=TS_WIFI_MODE_APSTA;
        assert(ts_net_manager_stop(TS_NET_IF_WIFI_STA)==ESP_OK&&wifi_sdk_mode==(ap?WIFI_MODE_AP:WIFI_MODE_STA)&&wifi_running==(bool)ap);
        assert(s_mode==(ap?TS_WIFI_MODE_AP:TS_WIFI_MODE_OFF)&&manager_state.wifi_sta_status.state==TS_NET_STATE_INITIALIZED);
    }
    record("STA_absent_role_or_NOT_STARTED_is_complete_AP_preserved_APSTA_moves_to_AP");

    for(unsigned failure=0;failure<5;failure++){
        prepare_custom_boot();wifi_running=true;wifi_sdk_mode=WIFI_MODE_APSTA;s_mode=TS_WIFI_MODE_APSTA;
        manager_state.wifi_sta_status.state=TS_NET_STATE_GOT_IP;wifi_failure=failure;
        esp_err_t expected=failure==0||failure==2?ESP_ERR_WIFI_NOT_INIT:ESP_FAIL;
        assert(ts_net_manager_stop(TS_NET_IF_WIFI_STA)==expected);
        for(unsigned stage=failure+1;stage<5;stage++)assert(!wifi_calls[stage]);
        assert(s_mode==TS_WIFI_MODE_APSTA&&manager_state.wifi_sta_status.state==TS_NET_STATE_ERROR);
    }
    for(unsigned target=0;target<2;target++)for(unsigned failure=2;failure<5;failure++){
        if(!target&&failure!=2)continue;
        prepare_custom_boot();wifi_running=true;wifi_sdk_mode=WIFI_MODE_STA;s_mode=TS_WIFI_MODE_STA;wifi_failure=failure;
        cJSON *params=cJSON_CreateObject();cJSON_AddStringToObject(params,"mode",target?"ap":"off");ts_api_result_t result={0};
        assert(api_network_wifi_mode(params,&result)==(failure==2?ESP_ERR_WIFI_NOT_INIT:ESP_FAIL));
        assert(result.code==TS_API_ERR_HARDWARE&&!result.data&&s_mode==TS_WIFI_MODE_STA);
        for(unsigned stage=failure+1;stage<5;stage++)assert(!wifi_calls[stage]);cJSON_Delete(params);ts_api_result_free(&result);
    }
    /* AP stop is another caller of the stricter shared mode helper. */
    for(unsigned failure=3;failure<5;failure++){
        prepare_custom_boot();wifi_running=true;wifi_sdk_mode=WIFI_MODE_APSTA;s_mode=TS_WIFI_MODE_APSTA;wifi_failure=failure;
        manager_state.wifi_sta_status.state=TS_NET_STATE_GOT_IP;manager_state.wifi_ap_status.state=TS_NET_STATE_GOT_IP;
        assert(ts_net_manager_stop(TS_NET_IF_WIFI_AP)==ESP_FAIL&&manager_state.wifi_ap_status.state==TS_NET_STATE_ERROR);
    }
    record("WiFi_mode_steps_and_API_AP_callers_preserve_failure_without_success_commit");

    for(unsigned scan_error=0;scan_error<2;scan_error++){
        prepare_custom_boot();manager_state.wifi_sta_status.state=manager_state.wifi_ap_status.state=TS_NET_STATE_INITIALIZED;
        scan_failure=scan_error;scan_cleanup_failure=true;console_text[0]=0;
        assert(do_wifi_scan(false)==1&&strstr(console_text,"Failed to stop temporary WiFi"));
        assert(wifi_calls[2]==2); /* Mode setup + one cleanup; no duplicate raw stop. */
    }
    record("WiFi_scan_CLI_reports_cleanup_error_on_success_and_failure_paths");
    setup(true);
}
