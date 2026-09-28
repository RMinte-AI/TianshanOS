#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/task.h"
#include "ts_ssh_service.h"
#include "ts_ssh_log_watch.h"
#include "ts_ssh_hosts_config.h"
#include "ts_keystore.h"
#include "ts_variable.h"
struct test_task {pthread_t thread;pthread_mutex_t lock;pthread_cond_t cond;unsigned notifications;void(*fn)(void*);void *arg;};
static _Thread_local TaskHandle_t self;
static TaskHandle_t tasks[256];static unsigned task_count;
static int replace_on_log;
static uint32_t cancel_on_connect, stale_finish; static int connect_count;
static char remote_identity[96]="abcd-1234:123:456";
static int fail_create;static _Atomic int ssh_live,remote_running,log_ready,fail_transport;
static pthread_mutex_t values_lock=PTHREAD_MUTEX_INITIALIZER;
static char value[32];
static ts_ssh_command_config_t command;
int64_t esp_timer_get_time(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (int64_t)t.tv_sec*1000000+t.tv_nsec/1000;}
void *heap_caps_malloc(size_t n,unsigned caps){return malloc(n);}
void *heap_caps_calloc(size_t n,size_t s,unsigned caps){return calloc(n,s);}
static void *task_entry(void*p){self=p;self->fn(self->arg);return NULL;}
BaseType_t xTaskCreatePinnedToCore(void(*fn)(void*),const char*name,unsigned stack,void*arg,unsigned priority,TaskHandle_t*out,unsigned core){assert(stack==8192&&priority==5&&core==1);if(fail_create)return 0;TaskHandle_t t=calloc(1,sizeof(*t));pthread_mutex_init(&t->lock,NULL);pthread_cond_init(&t->cond,NULL);t->fn=fn;t->arg=arg;*out=t;tasks[task_count++]=t;assert(!pthread_create(&t->thread,NULL,task_entry,t));return pdPASS;}
unsigned ulTaskNotifyTake(int clear,unsigned ms){pthread_mutex_lock(&self->lock);if(!self->notifications){struct timespec until;clock_gettime(CLOCK_REALTIME,&until);until.tv_sec+=ms/1000;until.tv_nsec+=(ms%1000)*1000000;until.tv_sec+=until.tv_nsec/1000000000;until.tv_nsec%=1000000000;while(!self->notifications&&pthread_cond_timedwait(&self->cond,&self->lock,&until)==0){}}unsigned n=self->notifications;self->notifications=0;pthread_mutex_unlock(&self->lock);return n;}
void xTaskNotifyGive(TaskHandle_t t){assert(t);pthread_mutex_lock(&t->lock);++t->notifications;pthread_cond_signal(&t->cond);pthread_mutex_unlock(&t->lock);}
void vTaskDelete(TaskHandle_t task){assert(!task);pthread_exit(NULL);}
static void join_tasks(void){for(unsigned i=0;i<task_count;i++){pthread_join(tasks[i]->thread,NULL);pthread_cond_destroy(&tasks[i]->cond);pthread_mutex_destroy(&tasks[i]->lock);free(tasks[i]);}task_count=0;assert(ssh_live==1);}
esp_err_t ts_variable_set_string(const char*key,const char*v){pthread_mutex_lock(&values_lock);snprintf(value,sizeof(value),"%s",v);pthread_mutex_unlock(&values_lock);return ESP_OK;}
esp_err_t ts_variable_set_int(const char*key,int32_t n){return ESP_OK;}
esp_err_t ts_ssh_hosts_config_get(const char*id,ts_ssh_host_config_t*out){memset(out,0,sizeof(*out));strcpy(out->id,id);strcpy(out->host,"192.0.2.8");strcpy(out->keyid,"synthetic-key");strcpy(out->username,"test");out->port=22;return ESP_OK;}
esp_err_t ts_ssh_commands_config_get(const char*id,ts_ssh_command_config_t*out){if(strcmp(id,command.id))return ESP_ERR_NOT_FOUND;*out=command;return ESP_OK;}
esp_err_t ts_ssh_commands_config_iterate(ts_ssh_cmd_iterator_cb_t cb,void*arg,size_t offset,size_t limit,size_t*total){cb(&command,0,arg);return ESP_OK;}
esp_err_t ts_ssh_commands_config_iterate_by_host(const char*host,ts_ssh_cmd_iterator_cb_t cb,void*arg,size_t offset,size_t limit,size_t*total){return ts_ssh_commands_config_iterate(cb,arg,offset,limit,total);}
esp_err_t ts_keystore_load_private_key(const char*id,char**key,size_t*len){*key=strdup("synthetic-key");*len=strlen(*key);return ESP_OK;}
struct ts_ssh_session_s {ts_ssh_config_t config;};
esp_err_t ts_ssh_session_create(const ts_ssh_config_t*cfg,ts_ssh_session_t*out){*out=malloc(sizeof(**out));(*out)->config=*cfg;++ssh_live;return ESP_OK;}
esp_err_t ts_ssh_session_destroy(ts_ssh_session_t s){free(s);--ssh_live;return ESP_OK;}
esp_err_t ts_ssh_connect(ts_ssh_session_t s){
 ++connect_count;
 if(cancel_on_connect){uint32_t g=cancel_on_connect;cancel_on_connect=0;ts_ssh_service_cancel_observation(command.id,g);}
 if(stale_finish){uint32_t g=stale_finish;stale_finish=0;ts_ssh_service_finish(command.id,g,"STARTED abcd-1234:123:456\n",s);ts_ssh_service_status_t st;ts_ssh_service_cached(command.id,&st);assert(st.busy);}
 return fail_transport?ESP_ERR_TIMEOUT:ESP_OK;
}
const char*ts_ssh_get_host(ts_ssh_session_t s){return "192.0.2.8";}
uint16_t ts_ssh_get_port(ts_ssh_session_t s){return 22;}
esp_err_t ts_ssh_exec(ts_ssh_session_t s,const char*cmd,ts_ssh_exec_result_t*r){memset(r,0,sizeof(*r));if(fail_transport)return ESP_ERR_TIMEOUT;const char *token;if(strstr(cmd,"grep -qF")){token=log_ready?"READY\n":"WAITING\n";if(replace_on_log){replace_on_log=0;strcpy(remote_identity,"abcd-1234:123:999");}}else if(strstr(cmd,"kill -TERM")){remote_running=0;token="STOPPED\n";}else {static char response[128];snprintf(response,sizeof(response),"RUNNING %s\n",remote_identity);token=remote_running?response:"STOPPED\n";}r->stdout_data=strdup(token);r->stdout_len=strlen(token);return ESP_OK;}
void ts_ssh_exec_result_free(ts_ssh_exec_result_t*r){free(r->stdout_data);free(r->stderr_data);memset(r,0,sizeof(*r));}
static void wait_done(ts_ssh_log_watch_handle_t h){int64_t end=esp_timer_get_time()+2000000;while(ts_ssh_log_watch_pending(h)&&esp_timer_get_time()<end){struct timespec t={0,1000000};nanosleep(&t,NULL);}assert(!ts_ssh_log_watch_pending(h));}

static void initialize_command(const char *id) {
 memset(&command,0,sizeof(command));strcpy(command.id,id);strcpy(command.host_id,"host");
 strcpy(command.name,"model");strcpy(command.var_name,"model");
 command.service_mode=command.nohup=command.enabled=true;
}
static void remove_stopped(void) {
 uint32_t ticket;ts_ssh_binding_lock();
 assert(!ts_ssh_service_command_protected(command.id));
 assert(ts_ssh_service_delete_begin(command.id,&ticket)==ESP_OK);
 uint32_t pin;assert(ts_ssh_service_pin(&command,"192.0.2.8",22,&pin)!=ESP_OK);
 ts_ssh_service_status_t cached;ts_ssh_service_cached(command.id,&cached);
 /* Old observer cannot revive a deleting entry. */
 ts_ssh_service_observe(command.id,cached.generation,"ready",command.var_name);
 ts_ssh_service_delete_finish(command.id,ticket,true);ts_ssh_binding_unlock();
}
static uint32_t launch(ts_ssh_session_t session) {
 uint32_t pin;assert(ts_ssh_service_pin(&command,"192.0.2.8",22,&pin)==ESP_OK);ts_ssh_service_set_owner(command.id,pin,"owner-rule");
 uint32_t gen;remote_running=0;assert(ts_ssh_service_begin(&command,session,&gen)==ESP_OK);
 remote_running=1;char receipt[128];snprintf(receipt,sizeof(receipt),"STARTED %s\n",remote_identity);assert(ts_ssh_service_finish(command.id,gen,receipt,session));ts_ssh_service_unpin(command.id,pin);return gen;
}
int main(void) {
 initialize_command("original");assert(ts_ssh_service_init()==ESP_OK);assert(ts_ssh_log_watch_init()==ESP_OK);
 ts_ssh_config_t cfg=TS_SSH_DEFAULT_CONFIG();ts_ssh_session_t session;assert(ts_ssh_session_create(&cfg,&session)==ESP_OK);
 ts_ssh_service_status_t st;
 uint32_t gen;remote_running=0;assert(ts_ssh_service_begin(&command,session,&gen)==ESP_OK);remote_running=1;
 assert(!ts_ssh_service_finish(command.id,gen,"STARTED abcd-1234:123:999\n",session));
 ts_ssh_service_cached(command.id,&st);assert(!strcmp(st.state,"unknown"));
 assert(ts_ssh_service_stop(command.id,&st)==ESP_OK);
 gen=launch(session);
 cancel_on_connect=gen;
 assert(ts_ssh_service_query(command.id,&st)==ESP_OK&&!st.busy&&!strcmp(st.state,"unknown"));
 assert(ts_ssh_service_query(command.id,&st)==ESP_OK&&!st.busy);
 assert(ts_ssh_service_stop(command.id,&st)==ESP_OK);
 /* More than the entire table's lifetime capacity, with same remote name. */
 for(unsigned n=0;n<70;n++) {
  assert(ts_ssh_service_query(command.id,&st)==ESP_OK&&!strcmp(st.state,"stopped"));
  remove_stopped();snprintf(command.id,sizeof(command.id),"replacement-%u",n);
 }
 assert(ts_ssh_service_query(command.id,&st)==ESP_OK);
 uint32_t oldpin,newpin,del;assert(ts_ssh_service_pin(&command,"192.0.2.8",22,&oldpin)==ESP_OK);
 ts_ssh_service_unpin(command.id,oldpin);
 ts_ssh_binding_lock();assert(ts_ssh_service_delete_begin(command.id,&del)==ESP_OK);
 ts_ssh_service_delete_finish(command.id,del,false); /* failed storage preserves entry */
 ts_ssh_binding_unlock();assert(ts_ssh_service_query(command.id,&st)==ESP_OK);
 remove_stopped();assert(ts_ssh_service_pin(&command,"192.0.2.8",22,&newpin)==ESP_OK&&newpin!=oldpin);
 ts_ssh_service_unpin(command.id,oldpin);assert(!ts_ssh_service_start_admissible(command.id));
 ts_ssh_service_delete_finish(command.id,del,true);assert(!ts_ssh_service_start_admissible(command.id));
 ts_ssh_service_unpin(command.id,newpin);assert(ts_ssh_service_start_admissible(command.id));
 gen=launch(session);stale_finish=gen-1;
 assert(ts_ssh_service_query(command.id,&st)==ESP_OK&&!st.busy);
 /* Existing independent registration protects even after command configuration disappears. */
 char id[32];strcpy(id,command.id);command.id[0]=0;
 assert(ts_ssh_service_command_protected(id));assert(ts_ssh_service_rule_protected("owner-rule"));strcpy(command.id,id);
 /* Ready evidence belongs to this launch and this physical instance. */
 assert(ts_ssh_service_verify_instance(command.id,gen,session)==ESP_OK);
 ts_ssh_service_observe(command.id,gen,"ready",command.var_name);
 fail_transport=1;
 assert(ts_ssh_service_query(command.id,&st)!=ESP_OK&&!st.busy);
 fail_transport=0;
 join_tasks(); /* recovery succeeds without any page or further query */
 assert(ts_ssh_service_cached(command.id,&st)==ESP_OK&&!strcmp(st.state,"ready"));
 int connections=connect_count;
 for(int i=0;i<100;i++) assert(ts_ssh_service_cached(command.id,&st)==ESP_OK);
 assert(connect_count==connections);
 /* A different start tick cannot inherit old log evidence. */
 strcpy(remote_identity,"abcd-1234:123:457");
 assert(ts_ssh_service_query(command.id,&st)==ESP_OK&&!strcmp(st.state,"running"));
 assert(ts_ssh_service_verify_instance(command.id,gen,session)!=ESP_OK);
 assert(ts_ssh_service_stop(command.id,&st)==ESP_OK);
 gen=launch(session);
 ts_ssh_log_watch_config_t watch={.run_generation=gen,.timeout_sec=2,.check_interval_ms=100};
 strcpy(watch.command_id,command.id);strcpy(watch.host_id,"host");strcpy(watch.var_name,"model");strcpy(watch.log_file,"/tmp/model.log");strcpy(watch.ready_pattern,"ready");
 log_ready=1;replace_on_log=1;assert(ts_ssh_log_watch_start(&watch,NULL)==ESP_OK);join_tasks();
 ts_ssh_service_cached(command.id,&st);assert(strcmp(st.state,"ready"));
 assert(ts_ssh_service_stop(command.id,&st)==ESP_OK);assert(!ts_ssh_service_rule_protected("owner-rule"));
 gen=launch(session);ts_ssh_service_observe(command.id,gen,"ready",command.var_name);
 fail_transport=1;connections=connect_count;
 assert(ts_ssh_service_query(command.id,&st)!=ESP_OK);
 join_tasks();assert(connect_count==connections+4); /* request + 3 lifetime attempts */
 fail_transport=0;
 for(int i=0;i<100;i++)ts_ssh_service_cached(command.id,&st);
 ts_ssh_service_recovery_kick(command.id);assert(task_count==0);
 assert(!strcmp(st.state,"unknown"));
 /* Explicit cancellation invalidates readiness, even after network returns. */
 ts_ssh_service_cancel_observation(command.id,gen);
 ts_ssh_service_observe(command.id,gen,"ready",command.var_name);
 assert(ts_ssh_service_query(command.id,&st)==ESP_OK&&!strcmp(st.state,"running"));
 assert(ts_ssh_service_stop(command.id,&st)==ESP_OK);
 ts_ssh_service_observe(command.id,gen,"ready",command.var_name);
 ts_ssh_service_cached(command.id,&st);assert(!strcmp(st.state,"stopped"));
 ts_ssh_session_destroy(session);assert(!ssh_live);
 puts("PASS service protocol: stale credentials, cancellation cleanup, deletion barrier/failure/reuse, 70 replacements, missing config protection, physical identity, page-free recovery, 3-attempt budget, zero-I/O cached reads");
}
