/* Real manager creation/start/restart, ETH event/task and DHCP entry functions.
 * Hardware, event delivery and NVS are explicit host models. No device proof. */
#include "ts_eth.h"
#include "cJSON.h"
#include "ts_net.h"
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
static TaskHandle_t s_dhcp_task;
static unsigned created_count, driver_stops, driver_starts;
static bool driver_running;
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
    driver_running=true;driver_starts++;
    event_queue[events_count++]=ETHERNET_EVENT_START;event_queue[events_count++]=ETHERNET_EVENT_CONNECTED;
    return ESP_OK;
}
static esp_err_t esp_eth_stop(esp_eth_handle_t h){
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
#include "eth_constants.inc"
#include "eth_creation.inc"
esp_err_t ts_eth_init(const ts_eth_config_t *config){
#include "eth_init_creation.inc"
    return s_eth_netif?ESP_OK:ESP_FAIL;
}
esp_netif_t *ts_eth_get_netif(void){return s_eth_netif;}
#include "eth_events.inc"
esp_err_t ts_net_mdns_start(void){return ESP_OK;}
static int manager_take(SemaphoreHandle_t mutex,int ticks){
    assert(!on_tcpip&&!project_lock_depth&&!manager_lock_depth);pthread_mutex_lock(mutex);manager_lock_depth++;return pdTRUE;
}
static void manager_give(SemaphoreHandle_t mutex){assert(manager_lock_depth==1);manager_lock_depth--;pthread_mutex_unlock(mutex);}
static SemaphoreHandle_t manager_create(void){return &manager_mutex;}
static esp_err_t load_eth_config_from_pins(ts_eth_config_t *config){*config=(ts_eth_config_t)TS_ETH_DEFAULT_CONFIG();return ESP_OK;}
static void manager_ip_event(void *arg,esp_event_base_t base,int32_t id,void *data){}
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
    created_count=driver_starts=driver_stops=0;events_count=0;driver_running=false;
    delayed_count=0;s_dhcp_task=NULL;started_notifications=stopped_notifications=0;
}
static int eth_boot_probe(void){
    setup(true);seed_network("192.168.50.1","192.168.50.254",TS_NET_IP_MODE_STATIC);seed_dhcp_eth("192.168.50");
    hardware_reset();assert(ts_net_manager_init()==ESP_OK);
    esp_err_t loaded=ts_dhcp_server_init();
    esp_err_t started=ts_net_manager_start(TS_NET_IF_ETH);deliver_events();run_delayed_task();
    printf("ETH boot: created=%u host=%08x load=%x start=%x DHCP=%u bindings=%zu\n",
        created_count,ntohl(created_host.ip.addr),loaded,started,netifs[1].status,servers[1].ts_bindings.count);
    int result=0;
    if(created_host.ip.addr!=inet_addr("192.168.50.1"))result=12;
    else if(loaded!=ESP_OK||started!=ESP_OK||netifs[1].status!=ESP_NETIF_DHCP_STARTED||servers[1].ts_bindings.count!=1)result=13;
    else if(packet(&servers[1],a,DHCPDISCOVER,0,0,DHCPS_STATE_OFFER)!=50)result=14;
    for(unsigned i=0;i<2;i++)drain(&servers[i]);
    return result;
}
#ifndef ETH_BOOT_REGRESSION
static void prepare_custom_boot(void){
    setup(true);seed_network("192.168.50.1","192.168.50.254",TS_NET_IP_MODE_STATIC);seed_dhcp_eth("192.168.50");
    hardware_reset();assert(ts_net_manager_init()==ESP_OK);
    assert(created_count==1&&created_host.ip.addr==inet_addr("192.168.50.1"));
    assert(ts_dhcp_server_init()==ESP_OK);
    assert(servers[1].ts_bindings.count==1&&s_state.eth_want_running);
    assert(ts_net_manager_start(TS_NET_IF_ETH)==ESP_OK);deliver_events();run_delayed_task();
    assert(netifs[1].status==ESP_NETIF_DHCP_STARTED);
}
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
    prepare_custom_boot();
    assert(packet(&servers[1],a,DHCPDISCOVER,0,0,DHCPS_STATE_OFFER)==50);
    assert(servers[1].server_address.addr==inet_addr("192.168.50.1"));
    assert(option_value(&servers[1],51,true)==htonl(7200));
    assert(created_host.ip.addr!=inet_addr("192.168.50.254")); /* Pool gateway is not host IP. */
    record("ETH_P1_real_manager_create_restore_start_first_OFFER_custom_subnet");

    link_down();assert(s_state.eth_want_running&&netifs[1].status==ESP_NETIF_DHCP_STOPPED);
    assert(ts_dhcp_server_load_config()==ESP_OK);assert(s_state.eth_want_running);
    link_up();run_delayed_task();assert(netifs[1].status==ESP_NETIF_DHCP_STARTED);
    record("ETH_disconnect_reload_reconnect_preserves_run_request");
    link_down();assert(ts_dhcp_server_stop(1)==ESP_OK);link_up();unsigned notices=started_notifications;
    run_delayed_task();assert(!s_state.eth_want_running&&netifs[1].status==ESP_NETIF_DHCP_STOPPED);
    assert(started_notifications==notices);
    record("ETH_user_stop_while_disconnected_wins_over_reconnect");
    assert(ts_dhcp_server_start(1)==ESP_OK);link_down();link_up();
    assert(delayed_count);assert(ts_dhcp_server_stop(1)==ESP_OK);notices=started_notifications;
    run_delayed_task();assert(!s_state.eth_want_running&&netifs[1].status==ESP_NETIF_DHCP_STOPPED&&started_notifications==notices);
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
    assert(driver_stops==stops&&stop_calls==sdk_stops&&s_state.eth_want_running&&netifs[1].status==ESP_NETIF_DHCP_STARTED);
    assert(!memcmp(&old,&servers[1].ts_bindings,sizeof(old))&&infos[1].ip.addr==inet_addr("192.168.50.1"));
    record("ETH_net_restart_invalid_candidate_preserves_interface_DHCP_request_and_table");
    /* Bindings are explicit edits on the current subnet; clearing is intentional. */
    assert(ts_dhcp_server_clear_static_bindings(1)==ESP_OK);prepare_pool("192.168.60");
    assert(ts_net_manager_restart(TS_NET_IF_ETH)==ESP_OK);deliver_events();run_delayed_task();
    assert(driver_stops==stops+1&&s_state.eth_want_running&&netifs[1].status==ESP_NETIF_DHCP_STARTED);
    assert(infos[1].ip.addr==inet_addr("192.168.60.1"));
    assert(packet(&servers[1],b,DHCPDISCOVER,0,0,DHCPS_STATE_OFFER)==100);
    record("ETH_net_restart_valid_complete_candidate_new_subnet_first_offer");
    assert(ts_dhcp_server_stop(1)==ESP_OK);
    assert(ts_net_manager_restart(TS_NET_IF_ETH)==ESP_OK);deliver_events();notices=started_notifications;run_delayed_task();
    assert(!s_state.eth_want_running&&netifs[1].status==ESP_NETIF_DHCP_STOPPED&&started_notifications==notices);
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
    assert(!strcmp(s_state.iface[1].applied_config.pool.start_ip,"192.168.50.100"));
    record("ETH_link_resume_uses_applied_config_and_preserves_pending_edits");
    prepare_custom_boot();netifs[1].up=false;
    unsigned no_dispatch=apply_dispatches;assert(ts_dhcp_server_eth_resume()==ESP_ERR_ESP_NETIF_IF_NOT_READY);
    assert(apply_dispatches==no_dispatch&&ts_dhcp_server_get_status(1,&status)==ESP_OK);
    assert(status.link_up&&status.want_running&&status.resume_error==ESP_ERR_ESP_NETIF_IF_NOT_READY);
    record("ETH_resume_distinguishes_netif_not_ready_from_user_stop_or_physical_link_down");
    prepare_custom_boot();link_down();
    s_state.iface[1].config_restore_error=ESP_ERR_NVS_TYPE_MISMATCH;
    link_up();unsigned dispatches=apply_dispatches;run_delayed_task();
    assert(apply_dispatches==dispatches&&s_state.eth_want_running);
    assert(ts_dhcp_server_get_status(1,&status)==ESP_OK&&status.resume_error==ESP_ERR_NVS_TYPE_MISMATCH);
    record("ETH_resume_blocks_storage_error_without_dispatch_and_reports_actual_reason");
    prepare_custom_boot();assert(ts_dhcp_server_stop(1)==ESP_OK);assert(ts_dhcp_server_load_config()==ESP_OK);
    assert(!s_state.eth_want_running&&netifs[1].status==ESP_NETIF_DHCP_STOPPED);
    notices=started_notifications;assert(ts_dhcp_server_eth_resume()==ESP_ERR_INVALID_STATE);
    assert(started_notifications==notices);
    record("ETH_explicit_stop_then_reload_does_not_rewrite_request_or_notify_start");
    boot_defaults(true);interface_up(0);unsigned stopped=stopped_notifications;
    assert(ts_dhcp_server_stop(0)==ESP_OK&&stopped_notifications==stopped+1);
    record("AP_auto_started_stop_notification_uses_real_SDK_state_outside_lock");
    setup(true); /* Release model leases and restore original test environment. */
}

#endif /* ETH_BOOT_REGRESSION */
