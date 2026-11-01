"""Real forward unit/task exits, native socketpairs and synthetic libssh2; no SSH/device."""
from pathlib import Path
import os
import re
from test_console_lifecycle import RTOS, body, run_native, IDF

root = Path.cwd()
source = Path(os.environ.get('TS_FORWARD_TEST_SOURCE', 'components/ts_security/src/ts_port_forward.c')).read_text()
rtos = RTOS.replace('static _Thread_local TaskHandle_t current_task;', r"""
static atomic_bool hold_entry, entry_waiting, join_waiting, stop_finished, stats_finished, stats_blocked;
static atomic_bool hold_create_return, create_return_waiting;
static atomic_int live_semaphores;static bool fail_binary;
static _Thread_local bool join_controller, stats_controller;
static _Thread_local TaskHandle_t current_task;
""")
rtos = rtos.replace('static void *task_entry(void *p) {current_task=p;',
                    'static void *task_entry(void *p) {current_task=p;entry_waiting=true;while(hold_entry)sched_yield();')
rtos = rtos.replace('    assert(s);pthread_mutex_lock(&s->mutex);struct timespec until=', '''    if(join_controller&&s->binary&&timeout)join_waiting=true;
    assert(s);pthread_mutex_lock(&s->mutex);if(stats_controller&&!s->binary&&!s->value)stats_blocked=true;struct timespec until=''', 1)
rtos = rtos.replace('SemaphoreHandle_t s=calloc(1,sizeof(*s));assert(s);', 'SemaphoreHandle_t s=calloc(1,sizeof(*s));assert(s);live_semaphores++;',1)
rtos = rtos.replace('static SemaphoreHandle_t xSemaphoreCreateBinary(void) {return semaphore(true);}',
                    'static SemaphoreHandle_t xSemaphoreCreateBinary(void) {if(fail_binary){fail_binary=false;return NULL;}return semaphore(true);}',1)
rtos = rtos.replace('pthread_cond_destroy(&s->changed);free(s);','pthread_cond_destroy(&s->changed);live_semaphores--;free(s);',1)
code = '#define _DARWIN_C_SOURCE\n' + rtos + r'''
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
'''
code += '#include "' + str(root / 'components/ts_security/include/ts_port_forward.h') + '"\n'
code += '#include "' + str(root / 'components/ch405labs_esp_libssh2/libssh2/include/libssh2.h') + '"\n'
code += r'''
#define TS_CALLOC_PSRAM calloc
#define TS_MALLOC_PSRAM malloc
#define TS_STRDUP_PSRAM strdup
static unsigned caps_created,caps_deleted,last_stack;
static struct {TaskHandle_t task;void *stack;} cap_records[256];
static int create_caps(void (*fn)(void *),const char *name,unsigned stack,void *arg,unsigned priority,TaskHandle_t *out,unsigned caps) {
    (void)name;(void)priority;assert(caps&(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));last_stack=stack;
    if(fail_next_create){fail_next_create=false;return pdFALSE;}
    TaskHandle_t task=calloc(1,sizeof(*task));assert(task&&task_count<128&&caps_created<256);
    task->fn=fn;task->arg=arg;tasks[task_count++]=task;
    cap_records[caps_created++]=(typeof(cap_records[0])){task,malloc(stack)};assert(cap_records[caps_created-1].stack);
    if(out)*out=task;assert(!pthread_create(&task->thread,NULL,task_entry,task));create_return_waiting=true;while(hold_create_return)sched_yield();return pdPASS;
}
static void delete_caps(TaskHandle_t task) {
    assert(!task&&current_task&&current_task->held==0);bool found=false;
    for(unsigned i=0;i<caps_created;i++)if(cap_records[i].task==current_task&&cap_records[i].stack){free(cap_records[i].stack);cap_records[i].stack=NULL;found=true;break;}
    assert(found);caps_deleted++;vTaskDelete(NULL);
}
#define xTaskCreateWithCaps create_caps
#define vTaskDeleteWithCaps delete_caps
struct _LIBSSH2_SESSION {int unused;};
struct _LIBSSH2_CHANNEL {size_t remote_at;};
struct ts_ssh_session_s {LIBSSH2_SESSION lib;int fd;bool destroyed;};
static struct ts_ssh_session_s ssh;
static int ssh_pair[2],client_peer=-1;
static atomic_int incoming_client=-1;
static atomic_int io_mode,open_waiting,write_waiting,send_waiting,read_calls,connected,disconnected;
enum {NORMAL,OPEN_AGAIN,WRITE_AGAIN,SEND_AGAIN,CONTINUOUS};
static char remote_bytes[4096],written_bytes[16384];
static size_t remote_len;
static atomic_size_t written_len;
static unsigned channel_allocated,channel_freed,listener_opened,listener_closed;
static int listener_fd=-1;static bool fail_bind;
static ts_port_forward_t tracked_forward;static bool tracked_started,forward_destroyed;
static void forward_free(void *p);
static size_t send_chunk;
static pthread_mutex_t io_lock=PTHREAD_MUTEX_INITIALIZER;
bool ts_ssh_is_connected(ts_ssh_session_t s) {return s&&!s->destroyed;}
LIBSSH2_SESSION *ts_ssh_get_libssh2_session(ts_ssh_session_t s) {assert(!s->destroyed);return &s->lib;}
int ts_ssh_get_socket(ts_ssh_session_t s) {assert(!s->destroyed);return s->fd;}
int libssh2_session_block_directions(LIBSSH2_SESSION *s) {(void)s;return LIBSSH2_SESSION_BLOCK_INBOUND;}
int libssh2_session_last_errno(LIBSSH2_SESSION *s) {(void)s;return LIBSSH2_ERROR_EAGAIN;}
int libssh2_session_last_error(LIBSSH2_SESSION *s,char **msg,int *len,int copy) {(void)s;(void)msg;(void)len;(void)copy;return LIBSSH2_ERROR_EAGAIN;}
LIBSSH2_CHANNEL *libssh2_channel_direct_tcpip_ex(LIBSSH2_SESSION *s,const char *host,int port,const char *origin,int sport) {
    (void)s;(void)origin;(void)sport;assert(!strcmp(host,"target.invalid")&&port==8080);
    if(io_mode==OPEN_AGAIN){open_waiting=true;return NULL;}
    channel_allocated++;return calloc(1,sizeof(LIBSSH2_CHANNEL));
}
ssize_t libssh2_channel_write_ex(LIBSSH2_CHANNEL *c,int stream,const char *buf,size_t len) {
    (void)c;(void)stream;if(io_mode==WRITE_AGAIN){write_waiting=true;return LIBSSH2_ERROR_EAGAIN;}
    pthread_mutex_lock(&io_lock);size_t n=len>2?2:len,at=written_len;assert(at+n<=sizeof(written_bytes));memcpy(written_bytes+at,buf,n);written_len=at+n;pthread_mutex_unlock(&io_lock);return n;
}
ssize_t libssh2_channel_read_ex(LIBSSH2_CHANNEL *c,int stream,char *buf,size_t len) {
    (void)stream;read_calls++;
    if(io_mode==CONTINUOUS){memcpy(buf,"x",1);return 1;}
    size_t n=remote_len-c->remote_at;if(n>len)n=len;if(!n)return LIBSSH2_ERROR_EAGAIN;
    memcpy(buf,remote_bytes+c->remote_at,n);c->remote_at+=n;return n;
}
int libssh2_channel_eof(LIBSSH2_CHANNEL *c) {(void)c;return 0;}
int libssh2_channel_close(LIBSSH2_CHANNEL *c) {(void)c;return 0;}
int libssh2_channel_free(LIBSSH2_CHANNEL *c) {channel_freed++;free(c);return 0;}
static int fake_socket(int domain,int type,int protocol) {assert(domain==AF_INET&&type==SOCK_STREAM&&!protocol);listener_opened++;listener_fd=socket(domain,type,protocol);return listener_fd;}
static int fake_bind(int fd,const struct sockaddr *addr,socklen_t size) {(void)size;assert(fd==listener_fd&&addr->sa_family==AF_INET);if(fail_bind){fail_bind=false;errno=EADDRINUSE;return -1;}return 0;}
static int fake_listen(int fd,int backlog) {assert(fd==listener_fd&&backlog==5);return 0;}
static int fake_accept(int fd,struct sockaddr *addr,socklen_t *size) {
    assert(fd==listener_fd);int client=atomic_exchange(&incoming_client,-1);if(client<0){errno=EAGAIN;return -1;}
    struct sockaddr_in from={.sin_family=AF_INET,.sin_port=htons(54321),.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    assert(*size>=sizeof(from));memcpy(addr,&from,sizeof(from));*size=sizeof(from);return client;
}
static ssize_t fake_send(int fd,const void *buf,size_t len,int flags) {
    if(io_mode==CONTINUOUS)return len;
    size_t n=send_chunk&&len>send_chunk?send_chunk:len;ssize_t sent=send(fd,buf,n,flags);
    if(sent<0&&(errno==EAGAIN||errno==EWOULDBLOCK))send_waiting=true;return sent;
}
static int fake_close(int fd) {if(fd==listener_fd){listener_closed++;listener_fd=-1;}return close(fd);}
#define free forward_free
#define socket fake_socket
#define bind fake_bind
#define listen fake_listen
#define accept fake_accept
#define send fake_send
#define close fake_close
'''
code += re.sub(r'^#include[^\n]*\n', '', source, flags=re.M)
code += r'''
#undef free
#undef socket
#undef bind
#undef listen
#undef accept
#undef send
#undef close
static void forward_free(void *p) {
    if(p&&p==tracked_forward){assert(!tracked_started||ts_port_forward_get_state(tracked_forward)==TS_FORWARD_STATE_STOPPED);forward_destroyed=true;tracked_forward=NULL;}
    free(p);
}
static void reset_fixture(int mode) {
    assert(!socketpair(AF_UNIX,SOCK_STREAM,0,ssh_pair));memset(&ssh,0,sizeof(ssh));ssh.fd=ssh_pair[0];
    io_mode=mode;incoming_client=-1;open_waiting=write_waiting=send_waiting=read_calls=0;
    connected=disconnected=0;remote_len=written_len=0;send_chunk=0;fail_bind=false;tracked_started=forward_destroyed=false;tracked_forward=NULL;hold_create_return=create_return_waiting=false;hold_entry=entry_waiting=join_waiting=stop_finished=stats_finished=stats_blocked=false;
}
static void finish_fixture(void) {
    join_tasks();assert(caps_created==caps_deleted);assert(channel_allocated==channel_freed);assert(!live_semaphores);
    if(client_peer>=0){close(client_peer);client_peer=-1;}close(ssh_pair[0]);close(ssh_pair[1]);assert(listener_fd<0);
}
static ts_port_forward_t make_forward(void) {
    ts_forward_config_t config=TS_FORWARD_DEFAULT_CONFIG();config.local_port=12345;config.remote_host="target.invalid";config.remote_port=8080;
    ts_port_forward_t forward=NULL;assert(ts_port_forward_create(&ssh,&config,&forward)==ESP_OK);tracked_forward=forward;return forward;
}
static esp_err_t begin_forward(ts_port_forward_t f) {esp_err_t ret=ts_port_forward_start(f);if(ret==ESP_OK)tracked_started=true;return ret;}
static void *stop_thread(void *arg) {join_controller=true;assert(ts_port_forward_stop(arg)==ESP_OK);stop_finished=true;return NULL;}
static void connect_callback(ts_port_forward_t f,const char *addr,uint16_t port,void *arg) {(void)f;assert(!strcmp(addr,"127.0.0.1")&&port==54321&&arg==(void*)1);connected++;}
static void disconnect_callback(ts_port_forward_t f,uint64_t bytes,void *arg) {(void)f;assert(bytes==11&&arg==(void*)2);disconnected++;}
static void queue_client(void) {int pair[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));client_peer=pair[1];incoming_client=pair[0];}
static void early_stop(void) {
    reset_fixture(NORMAL);hold_entry=true;ts_port_forward_t f=make_forward();assert(begin_forward(f)==ESP_OK);WAIT_UNTIL(entry_waiting);
    pthread_t stop;assert(!pthread_create(&stop,NULL,stop_thread,f));WAIT_UNTIL(join_waiting||stop_finished);
    assert(join_waiting&&!stop_finished); /* old IDLE-state shortcut returns here before task entry */
    hold_entry=false;assert(!pthread_join(stop,NULL));assert(stop_finished&&ts_port_forward_get_state(f)==TS_FORWARD_STATE_STOPPED);
    assert(ts_port_forward_destroy(f)==ESP_OK);finish_fixture();puts("PASS immediate stop joins even before first worker entry, then destroy/session ownership is safe");
}
static bool stats_match(ts_port_forward_t f,uint64_t sent,uint64_t received) {
    ts_forward_stats_t stats;assert(ts_port_forward_get_stats(f,&stats)==ESP_OK);return stats.bytes_sent==sent&&stats.bytes_received==received;
}
static void normal_transfer(void) {
    reset_fixture(NORMAL);ts_port_forward_t f=make_forward();
    assert(ts_port_forward_set_connect_cb(f,connect_callback,(void*)1)==ESP_OK);
    assert(ts_port_forward_set_disconnect_cb(f,disconnect_callback,(void*)2)==ESP_OK);
    for(unsigned round=0;round<2;round++) {
        remote_len=4;memcpy(remote_bytes,"by\0e",4);written_len=0;send_chunk=2;queue_client();
        assert(begin_forward(f)==ESP_OK);assert(ts_port_forward_start(f)==ESP_OK);WAIT_UNTIL(connected==(int)round+1);
        assert(send(client_peer,"hel\0lo!",7,0)==7);WAIT_UNTIL(written_len==7);assert(!memcmp(written_bytes,"hel\0lo!",7));
        char reply[4];assert(recv(client_peer,reply,sizeof(reply),MSG_WAITALL)==4&&!memcmp(reply,"by\0e",4));
        WAIT_UNTIL(stats_match(f,7*(round+1),4*(round+1)));
        close(client_peer);client_peer=-1;WAIT_UNTIL(disconnected==(int)round+1);
        assert(ts_port_forward_stop(f)==ESP_OK&&ts_port_forward_stop(f)==ESP_OK);join_tasks();
        ts_forward_stats_t stats;assert(ts_port_forward_get_stats(f,&stats)==ESP_OK);
        assert(stats.active_connections==0&&stats.total_connections==round+1&&stats.state==TS_FORWARD_STATE_STOPPED);
    }
    assert(ts_port_forward_destroy(f)==ESP_OK);finish_fixture();
    printf("PASS binary/partial bidirectional transfer, callbacks/stats, stop/start reuse; requested stack=%u bytes\n",last_stack);
}
static void multiple_connections(void) {
    reset_fixture(NORMAL);remote_len=4;memcpy(remote_bytes,"by\0e",4);send_chunk=2;
    ts_port_forward_t f=make_forward();ts_port_forward_set_connect_cb(f,connect_callback,(void*)1);ts_port_forward_set_disconnect_cb(f,disconnect_callback,(void*)2);
    queue_client();int first=client_peer;assert(begin_forward(f)==ESP_OK);WAIT_UNTIL(connected==1);
    queue_client();int second=client_peer;WAIT_UNTIL(connected==2);
    char reply[4];assert(recv(first,reply,4,MSG_WAITALL)==4&&!memcmp(reply,"by\0e",4));assert(recv(second,reply,4,MSG_WAITALL)==4&&!memcmp(reply,"by\0e",4));
    assert(send(first,"hel\0lo!",7,0)==7&&send(second,"hel\0lo!",7,0)==7);WAIT_UNTIL(written_len==14);WAIT_UNTIL(stats_match(f,14,8));
    close(first);WAIT_UNTIL(disconnected==1);ts_forward_stats_t stats;assert(ts_port_forward_get_stats(f,&stats)==ESP_OK);
    assert(stats.active_connections==1&&stats.total_connections==2&&stats.bytes_sent==14&&stats.bytes_received==8);
    close(second);client_peer=-1;WAIT_UNTIL(disconnected==2);
    assert(ts_port_forward_stop(f)==ESP_OK&&ts_port_forward_destroy(f)==ESP_OK);finish_fixture();
    puts("PASS two live connections preserve list/callbacks and 64-bit active-plus-closed statistics across independent closes");
}
static void *stats_thread(void *arg) {stats_controller=true;ts_forward_stats_t stats;assert(ts_port_forward_get_stats(arg,&stats)==ESP_OK);stats_finished=true;return NULL;}
static void ts_console_printf(const char *format,...) {(void)format;}
static void ts_console_clear_interrupt(void) {}
esp_err_t ts_ssh_disconnect(ts_ssh_session_t session) {assert(forward_destroyed);session->destroyed=true;return ESP_OK;}
esp_err_t ts_ssh_session_destroy(ts_ssh_session_t session) {assert(session->destroyed&&forward_destroyed);return ESP_OK;}
static int cli_cleanup(ts_port_forward_t forward,ts_ssh_session_t session) {
 esp_err_t ret;
CLI_CLEANUP_BODY
}
static void blocked_transfer(int mode) {
    reset_fixture(mode);queue_client();
    if(mode==WRITE_AGAIN)assert(send(client_peer,"hello",5,0)==5);
    if(mode==SEND_AGAIN) {
        remote_len=4096;memset(remote_bytes,'r',remote_len);int fd=incoming_client;set_nonblocking(fd);
        char filler[4096];memset(filler,'z',sizeof(filler));while(send(fd,filler,sizeof(filler),MSG_DONTWAIT)>0){}assert(errno==EAGAIN||errno==EWOULDBLOCK);
    }
    ts_port_forward_t f=make_forward();assert(begin_forward(f)==ESP_OK);
    if(mode==OPEN_AGAIN)WAIT_UNTIL(open_waiting);
    if(mode==WRITE_AGAIN)WAIT_UNTIL(write_waiting);
    if(mode==SEND_AGAIN&&!getenv("TS_SKIP_VERBOSE_QUERY")) {
        WAIT_UNTIL(send_waiting);pthread_t stats;assert(!pthread_create(&stats,NULL,stats_thread,f));WAIT_UNTIL(stats_finished||stats_blocked);
        assert(stats_finished&&!stats_blocked);assert(!pthread_join(stats,NULL)); /* actual verbose caller can get back to UART */
    }
    if(mode==SEND_AGAIN)WAIT_UNTIL(send_waiting);
    if(mode==CONTINUOUS)WAIT_UNTIL(read_calls>10);
    assert(cli_cleanup(f,&ssh)==0);finish_fixture();
    printf("PASS blocked mode%d: actual CLI stop/destroy precedes SSH release; no forced task delete\n",mode);
}
static atomic_bool start_finished;
static void *start_thread(void *arg) {assert(begin_forward(arg)==ESP_OK);start_finished=true;return NULL;}
static void creation_handoff(void) {
    reset_fixture(NORMAL);queue_client();ts_port_forward_t f=make_forward();hold_create_return=true;start_finished=false;
    pthread_t creator;assert(!pthread_create(&creator,NULL,start_thread,f));WAIT_UNTIL(create_return_waiting&&entry_waiting);
    assert(!connected&&!start_finished);hold_create_return=false;assert(!pthread_join(creator,NULL));
    assert(cli_cleanup(f,&ssh)==0);finish_fixture();puts("PASS task scheduled before creator returns cannot use unpublished state/handle");
}
static atomic_bool tail_paused,release_tail;
static void pause_tail(SemaphoreHandle_t s) {(void)s;tail_paused=true;while(!release_tail)sched_yield();}
static void tail_handoff(void) {
    reset_fixture(NORMAL);tail_paused=release_tail=false;after_binary_give=pause_tail;
    ts_port_forward_t f=make_forward();assert(begin_forward(f)==ESP_OK);pthread_t stopper;assert(!pthread_create(&stopper,NULL,stop_thread,f));
    WAIT_UNTIL(tail_paused&&join_waiting);assert(!stop_finished&&f->task_handle);
    assert(ts_port_forward_destroy(f)==ESP_ERR_NOT_FINISHED&&!forward_destroyed);
    assert(ts_port_forward_start(f)==ESP_ERR_INVALID_STATE); /* old worker still owns its last mutex access */
    release_tail=true;assert(!pthread_join(stopper,NULL));after_binary_give=NULL;
    assert(ts_port_forward_destroy(f)==ESP_OK);finish_fixture();puts("PASS exit signal before last unlock cannot release context/SSH or revive old worker");
}
static atomic_bool self_stop_seen;
static void self_stop_callback(ts_port_forward_t f,const char *addr,uint16_t port,void *arg) {
    (void)addr;(void)port;(void)arg;assert(ts_port_forward_stop(f)==ESP_ERR_INVALID_STATE);assert(ts_port_forward_destroy(f)==ESP_ERR_INVALID_STATE);self_stop_seen=true;
}
static void failure_and_self_stop(void) {
    reset_fixture(NORMAL);fail_binary=true;ts_port_forward_t failed=NULL;
    ts_forward_config_t config=TS_FORWARD_DEFAULT_CONFIG();config.local_port=12345;config.remote_host="target.invalid";config.remote_port=8080;
    assert(ts_port_forward_create(&ssh,&config,&failed)==ESP_ERR_NO_MEM&&!failed&&!live_semaphores);finish_fixture();
    reset_fixture(NORMAL);ts_port_forward_t bind_failed=make_forward();fail_bind=true;
    assert(begin_forward(bind_failed)==ESP_FAIL&&listener_fd<0);assert(ts_port_forward_destroy(bind_failed)==ESP_OK);finish_fixture();
    reset_fixture(NORMAL);ts_port_forward_t f=make_forward();fail_next_create=true;
    assert(begin_forward(f)==ESP_ERR_NO_MEM&&ts_port_forward_get_state(f)==TS_FORWARD_STATE_ERROR&&listener_fd<0);
    queue_client();self_stop_seen=false;assert(ts_port_forward_set_connect_cb(f,self_stop_callback,NULL)==ESP_OK);assert(begin_forward(f)==ESP_OK);WAIT_UNTIL(self_stop_seen);
    assert(cli_cleanup(f,&ssh)==0);finish_fixture();puts("PASS create failure cleans listener/retry works; self stop/destroy refuse freeing live context");
}

'''
cli = Path('components/ts_console/commands/ts_cmd_ssh.c').read_text()
start = cli.index('    ts_console_printf("Stopping port forward...')
end = cli.index('\n}', start)
code = code.replace('CLI_CLEANUP_BODY', cli[start:end])
# Exercise the other three actual temporary task bodies, with hardware/queue leaves fake.
code += '#include "cJSON.h"\n#include "' + str(root / 'components/ts_drivers/include/ts_device_ctrl.h') + '"\n'
code += r'''
static unsigned virtual_delays,dhcp_starts,ping_calls,gpio_pulses;
static void caps_delay(unsigned ticks) {virtual_delays+=ticks;}
#define vTaskDelay caps_delay
typedef int esp_netif_t;
#define TS_DHCP_IF_ETH 1
#define ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED 2
static bool s_link_up;
static TaskHandle_t s_dhcp_task,s_lpmu_detect_task;
static esp_err_t ts_dhcp_server_start(int iface) {assert(iface==TS_DHCP_IF_ETH);dhcp_starts++;return ESP_OK;}
#define LPMU_DEFAULT_IP "synthetic-only"
#define TS_EVENT_BASE_DEVICE_MON 3
#define TS_EVENT_DEVICE_STATUS_CHANGED 77
static struct {ts_device_state_t state;uint64_t power_on_time;unsigned boot_count;struct{void *power_btn;} gpio;} s_lpmu;
static unsigned ping_succeeds_at;
static bool ping_host(const char *host,unsigned timeout) {assert(!strcmp(host,LPMU_DEFAULT_IP)&&timeout==1000);return ++ping_calls==ping_succeeds_at;}
static esp_err_t ts_gpio_set_level(void *gpio,int level) {assert(gpio==(void*)1&&(level==0||level==1));gpio_pulses++;return ESP_OK;}
const char *ts_device_state_to_str(ts_device_state_t state) {return state==TS_DEVICE_STATE_ON?"on":"off";}
'''
code += body('components/ts_net/src/ts_eth.c','dhcp_start_task')
code += body('components/ts_drivers/src/ts_device_ctrl.c','lpmu_startup_detect_task')
code += '#include "' + str(root / 'components/ts_automation/include/ts_action_manager.h') + '"\n'
action_source=Path('components/ts_automation/src/ts_action_manager.c').read_text()
action_ctx_end=action_source.index('} action_manager_ctx_t;')+len('} action_manager_ctx_t;')
action_ctx_start=action_source.rfind('typedef struct {',0,action_ctx_end)
code += 'typedef void *QueueHandle_t;\n' + action_source[action_ctx_start:action_ctx_end]
code += r'''
static action_manager_ctx_t action_ctx,*s_ctx=&action_ctx;
static int xQueueReceive(void *queue,void *entry,unsigned timeout) {(void)entry;assert(queue==(void*)1&&timeout==1000);s_ctx->running=false;return pdFALSE;}
void ts_action_snapshot_release(ts_auto_action_t *action) {(void)action;assert(false);}
static void completion_release(void *completion) {(void)completion;assert(false);}
static void entry_finished(ts_action_queue_entry_t *entry,esp_err_t ret) {(void)entry;(void)ret;assert(false);}
static esp_err_t execute_entry(ts_action_queue_entry_t *entry,ts_action_result_t *result) {(void)entry;(void)result;assert(false);return ESP_FAIL;}
'''
code += body('components/ts_automation/src/ts_action_manager.c','action_executor_task')
code += r'''
#undef vTaskDelay
static void other_caps_exits(const char *kind) {
    esp_netif_t netif=1;
    if(!kind||!strcmp(kind,"cap_dhcp"))for(unsigned linked=0;linked<2;linked++) {
        unsigned before=dhcp_starts;s_link_up=linked;
        assert(create_caps(dhcp_start_task,"dhcp_start",4096,&netif,5,&s_dhcp_task,MALLOC_CAP_SPIRAM)==pdPASS);
        join_tasks();assert(!s_dhcp_task&&dhcp_starts==before+linked&&caps_created==caps_deleted);
    }
    if(!kind||!strcmp(kind,"cap_lpmu"))for(unsigned scenario=0;scenario<3;scenario++) {
        memset(&s_lpmu,0,sizeof(s_lpmu));s_lpmu.gpio.power_btn=(void*)1;ping_calls=gpio_pulses=0;
        ping_succeeds_at=scenario==0?1:scenario==1?4:0;
        assert(create_caps(lpmu_startup_detect_task,"lpmu_detect",4096,NULL,5,&s_lpmu_detect_task,MALLOC_CAP_SPIRAM)==pdPASS);
        join_tasks();assert(!s_lpmu_detect_task&&caps_created==caps_deleted);
        assert(s_lpmu.state==(scenario==2?TS_DEVICE_STATE_OFF:TS_DEVICE_STATE_ON));assert(gpio_pulses==(scenario==0?0:2));
    }
    if(!kind||!strcmp(kind,"cap_action")) {
        memset(&action_ctx,0,sizeof(action_ctx));action_ctx.running=true;action_ctx.action_queue=(void*)1;action_ctx.stats_mutex=xSemaphoreCreateMutex();
        assert(create_caps(action_executor_task,"action_exec",16384,NULL,5,&action_ctx.executor_task,MALLOC_CAP_8BIT)==pdPASS);
        join_tasks();assert(!action_ctx.executor_task&&caps_created==caps_deleted);vSemaphoreDelete(action_ctx.stats_mutex);
    }
    puts("PASS actual DHCP link/no-link, LPMU online/boot/timeout and idle action-executor task exits reclaim their WithCaps stack allocations");
}
int main(void) {
    const char *kind=getenv("TS_FORWARD_CASE");
    if(kind&&!strncmp(kind,"blocked",7)){blocked_transfer(atoi(kind+7));return 0;}
    if(kind&&!strcmp(kind,"cap_forward")){early_stop();return 0;}
    if(kind&&!strncmp(kind,"cap_",4)){other_caps_exits(kind);return 0;}
    early_stop();normal_transfer();multiple_connections();for(int mode=OPEN_AGAIN;mode<=CONTINUOUS;mode++)blocked_transfer(mode);
    creation_handoff();tail_handoff();failure_and_self_stop();other_caps_exits(NULL);return 0;
}
'''
if os.environ.get('TS_CAPS_DELETE_BASELINE'):
    code=code.replace('vTaskDeleteWithCaps(NULL);','vTaskDelete(NULL);')
# Whole production forward unit plus actual task bodies; platform I/O/SSH only faked.
headers={'freertos/FreeRTOS.h':'','freertos/semphr.h':'',
         'ts_automation_types.h':'#include "'+str(root/'components/ts_automation/include/ts_automation_types.h')+'"\n',
         'ts_ssh_service.h':'#include "'+str(root/'components/ts_security/include/ts_ssh_service.h')+'"\n'}
run_native(code, extra_sources=[IDF/'components/json/cJSON/cJSON.c'],extra_headers=headers)
