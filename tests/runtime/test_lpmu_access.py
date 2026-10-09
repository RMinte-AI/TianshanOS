#!/usr/bin/env python3
"""Exercise production LPMU API/task with synthetic SSH, no device writes."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
idf = Path(os.environ.get('IDF_PATH', '/Users/massif/esp/v5.5.2/esp-idf'))

def extract(path, name):
    source = (root / path).read_text()
    match = re.search(r'^(?:static )?[^\n;]+\b' + name + r'\([^;]+?\)\s*\{', source, re.M)
    assert match, name
    return source[match.start():source.index('\n}', match.start()) + 2] + '\n'

code = r'''
#define _POSIX_C_SOURCE 200809L
#include "platform.h"
#include <stdarg.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ts_api.h"
#include "ts_ssh_client.h"
#include "ts_ssh_hosts_config.h"
#include "ts_known_hosts.h"
#include "ts_scp.h"
#define TS_STRDUP_PSRAM strdup
#define LPMU_HOST "10.10.99.99"
#define LPMU_PORT 22
#define LPMU_REMOTE_TARBALL "lpmu-agx-network-setup.tar.gz"
#define LPMU_STACK_SIZE 8192
#define LPMU_OUTPUT_TAIL_MAX 1024
#define LPMU_LOG_PATH "TEST_LOG_PATH"
#define LPMU_LOG_CHUNK 1024
#define TS_MALLOC_PSRAM_ONLY log_malloc
#define pdPASS 1
#define TS_USB_MUX_LPMU 1
typedef int BaseType_t;
typedef void *TaskHandle_t;
static uint8_t lpmu_archive_start[12874];
#define lpmu_archive_end (lpmu_archive_start + sizeof(lpmu_archive_start))
static StaticSemaphore_t mutex;
static SemaphoreHandle_t xSemaphoreCreateMutex(void) { return xSemaphoreCreateMutexStatic(&mutex); }
static void (*pending_task)(void *);
static void *pending_arg, *secret_buffer;
static size_t secret_size;
static int64_t monotonic_us;
static int64_t esp_timer_get_time(void) {monotonic_us+=7;return monotonic_us;}
static int task_starts, secret_wipes, script_runs, uploads, active_sessions;
static bool allocation_failure, task_failure, configured = true, missing_project;
static bool sd_failure, log_read_allocation_failure;
static bool log_close_failure;
static bool task_executing, log_write_failure, log_short_write;
static unsigned uxTaskGetStackHighWaterMark(TaskHandle_t task) {assert(!task && task_executing);return 1536;}
static bool log_open_failure, log_read_failure, log_short_read, log_seek_failure;
static bool replace_run_during_read;
static int remote_extract_exit, remote_chmod_exit, upload_error;
static int io_observations, log_read_calls;
static void observe_io(void);
static int test_open(const char *path, int flags, ...);
static ssize_t test_write(int fd, const void *data, size_t len);
static ssize_t test_read(int fd, void *data, size_t len);
static off_t test_lseek(int fd, off_t pos, int whence);
static int test_fstat(int fd, struct stat *info);
static int test_close(int fd);
static int cleanup_observations, log_close_observations;
static void observe_cleanup(void);
static int mux_target = TS_USB_MUX_LPMU, connection_error, host_status = TS_HOST_VERIFY_OK;
static int script_exit, script_error;
static const char *script_stdout, *script_stderr;
static size_t output_chunk = 512;
static const char *secret = " '$(synthetic);\\密码 ";
static void *log_malloc(size_t n) { assert(n==LPMU_LOG_CHUNK+1);return log_read_allocation_failure?NULL:malloc(n); }
static esp_err_t ts_storage_mkdir_p(const char *path) {
    assert(task_executing);observe_io();return sd_failure?ESP_FAIL:ESP_OK;
}
static void *test_malloc(size_t n) {
    if (allocation_failure) return NULL;
    assert(!secret_buffer); secret_buffer=malloc(n);secret_size=n;return secret_buffer;
}
static void test_free(void *p) {
    if (p && p==secret_buffer) {
        for(size_t i=0;i<secret_size;i++) assert(((unsigned char*)p)[i]==0);
        secret_wipes++;secret_buffer=NULL;
    }
    free(p);
}
static BaseType_t xTaskCreate(void (*fn)(void*),const char *name,unsigned size,void *arg,int priority,TaskHandle_t *handle) {
    ++task_starts;if(task_failure)return 0;
    pending_task=fn;pending_arg=arg;*handle=(void*)1;return pdPASS;
}
static void vTaskDelete(void *task) {}
static bool ts_usb_mux_is_configured(void) { return configured; }
static int ts_usb_mux_get_target(void) { return mux_target; }
esp_err_t ts_ssh_hosts_config_find(const char *host,uint16_t port,const char *username,ts_ssh_host_config_t *cfg) {
    assert(!strcmp(host,LPMU_HOST));strcpy(cfg->host,host);cfg->port=port;cfg->enabled=true;
    strcpy(cfg->username,"synthetic");strcpy(cfg->keyid,"test-key");cfg->auth_type=TS_SSH_HOST_AUTH_KEY;return ESP_OK;
}
static esp_err_t ts_keystore_load_private_key(const char *id,char **data,size_t *len) {
    *data=strdup("synthetic-private-key");*len=strlen(*data);return ESP_OK;
}
esp_err_t ts_ssh_session_create(const ts_ssh_config_t *cfg,ts_ssh_session_t *out) {
    assert(cfg->auth_method==TS_SSH_AUTH_PUBLICKEY);*out=(ts_ssh_session_t)1;++active_sessions;return ESP_OK;
}
esp_err_t ts_ssh_connect(ts_ssh_session_t s) { return connection_error; }
esp_err_t ts_ssh_disconnect(ts_ssh_session_t s) { observe_cleanup(); return ESP_OK; }
esp_err_t ts_ssh_session_destroy(ts_ssh_session_t s) { --active_sessions;return ESP_OK; }
const char *ts_ssh_get_error(ts_ssh_session_t s) { return "synthetic transport error"; }
esp_err_t ts_known_hosts_verify(ts_ssh_session_t s,ts_host_verify_result_t *out,ts_known_host_t *host) {
    *out=host_status;return ESP_OK;
}
esp_err_t ts_ssh_exec_stream(ts_ssh_session_t s,const char *cmd,ts_ssh_output_cb_t cb,void *ctx,int *exit) {
    assert(!strstr(cmd,secret));*exit=0;
    if(strstr(cmd,"test -f")){assert(strstr(cmd,"$HOME/lpmu-agx-network-setup/lpmu/setup-smart-route.sh"));*exit=missing_project?1:0;}
    if(strstr(cmd,"tar -xzf")){assert(strstr(cmd,"-C \"$HOME/lpmu-agx-network-setup\" --strip-components=1"));*exit=remote_extract_exit;}
    if(strstr(cmd,"chmod")){assert(strstr(cmd,"$HOME/lpmu-agx-network-setup/lpmu/"));*exit=remote_chmod_exit;}
    return ESP_OK;
}
esp_err_t ts_scp_send_buffer(ts_ssh_session_t s,const uint8_t *data,size_t len,const char *path,int mode) {
    assert(len==12874 && !strcmp(path,LPMU_REMOTE_TARBALL));++uploads;return upload_error;
}
static void emit(ts_ssh_output_cb_t cb,void *ctx,const char *text,bool stderr_stream) {
    if(!text)return;size_t len=strlen(text);
    while(len){size_t n=len<output_chunk?len:output_chunk;cb(text,n,stderr_stream,ctx);text+=n;len-=n;}
}
esp_err_t ts_ssh_exec_stream_input(ts_ssh_session_t s,const char *cmd,const char *input,size_t len,ts_ssh_output_cb_t cb,void *ctx,int *exit) {
    ++script_runs;assert(host_status==TS_HOST_VERIFY_OK);
    assert(strstr(cmd,"cd \"$HOME/lpmu-agx-network-setup/lpmu\" && sudo -S -k -p '' -- ./setup-smart-route.sh"));
    assert(!strstr(cmd,secret));assert(len==strlen(secret)+1 && !memcmp(input,secret,len-1) && input[len-1]=='\n');
    emit(cb,ctx,script_stderr,true);emit(cb,ctx,script_stdout,false);*exit=script_exit;return script_error;
}
'''
for name in ['ts_api_result_free', 'ts_api_result_ok', 'ts_api_result_error']:
    code += extract('components/ts_api/src/ts_api.c', name)
source = (root / 'components/ts_api/src/ts_api_lpmu_access.c').read_text()
code += '#define malloc test_malloc\n#define free test_free\n'
code += '#define open test_open\n#define write test_write\n#define read test_read\n#define lseek test_lseek\n#define fstat test_fstat\n#define close test_close\n'
code += source[source.index('typedef enum {'):source.index('static const ts_api_endpoint_t lpmu_access_endpoints[]')]
code += '#undef malloc\n#undef free\n'
code += '#undef open\n#undef write\n#undef read\n#undef lseek\n#undef fstat\n#undef close\n'
code += r'''
static void observe_io(void) {
    assert(pthread_mutex_trylock(&mutex)==0);pthread_mutex_unlock(&mutex);
    ts_api_result_t r={0};assert(api_lpmu_access_status(NULL,&r)==ESP_OK);
    ts_api_result_free(&r);io_observations++;
}
static int test_open(const char *path,int flags,...) {
    observe_io();if(log_open_failure)return -1;
    if(flags&O_CREAT){va_list args;va_start(args,flags);int mode=va_arg(args,int);va_end(args);return open(path,flags,mode);}
    return open(path,flags);
}
static ssize_t test_write(int fd,const void *data,size_t len) {
    observe_io();if(log_write_failure)return -1;
    return write(fd,data,log_short_write && len ? len-1 : len);
}
static ssize_t test_read(int fd,void *data,size_t len) {
    observe_io();assert(len<=LPMU_LOG_CHUNK);log_read_calls++;
    if(replace_run_during_read) {
        replace_run_during_read=false;
        cJSON *p=cJSON_CreateObject();cJSON_AddStringToObject(p,"sudo_password",secret);ts_api_result_t r={0};
        assert(api_lpmu_access_start(p,&r)==ESP_OK);ts_api_result_free(&r);cJSON_Delete(p);
        script_stdout="replacement output\n✓ 互联网连接正常！\n=== 配置完成 ===\n";
        void *arg=pending_arg;pending_arg=NULL;task_executing=true;pending_task(arg);task_executing=false;
        assert(!secret_buffer && !active_sessions && !s_status.running);
    }
    if(log_read_failure)return -1;
    return read(fd,data,log_short_read && len ? len-1 : len);
}
static off_t test_lseek(int fd,off_t pos,int whence) {observe_io();return log_seek_failure?-1:lseek(fd,pos,whence);}
static int test_fstat(int fd,struct stat *info) {observe_io();return fstat(fd,info);}
static int test_close(int fd) {
    observe_io();bool run_log=(intptr_t)fd==(intptr_t)s_run_log;
    if(run_log){observe_cleanup();log_close_observations++;}
    int ret=close(fd);return run_log && log_close_failure ? -1 : ret;
}
static void observe_cleanup(void) {
    ts_api_result_t r={0};assert(api_lpmu_access_status(NULL,&r)==ESP_OK);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(r.data,"running")));
    const char *stage=cJSON_GetObjectItem(r.data,"stage")->valuestring;
    assert(strcmp(stage,"success") && strcmp(stage,"failed"));
    ts_api_result_free(&r);
    cJSON *p=cJSON_CreateObject();cJSON_AddNumberToObject(p,"run_id",s_status.run_id);
    assert(api_lpmu_access_log(p,&r)==ESP_ERR_INVALID_STATE && r.code==TS_API_ERR_BUSY);
    ts_api_result_free(&r);cJSON_Delete(p);
    p=cJSON_Parse("{\"sudo_password\":\"second\"}");
    assert(api_lpmu_access_start(p,&r)==ESP_ERR_INVALID_STATE && r.code==TS_API_ERR_BUSY);
    ts_api_result_free(&r);cJSON_Delete(p);cleanup_observations++;
}
static void start_run(void) {
    cJSON *p=cJSON_CreateObject();cJSON_AddStringToObject(p,"sudo_password",secret);
    ts_api_result_t r={0};assert(api_lpmu_access_start(p,&r)==ESP_OK && r.code==TS_API_OK);
    assert(pending_task && pending_arg);cJSON_Delete(p);ts_api_result_free(&r);
}
static void finish_run(void) { void *arg=pending_arg;pending_arg=NULL;task_executing=true;pending_task(arg);task_executing=false;assert(!secret_buffer && !active_sessions && !s_status.running && (intptr_t)s_run_log==-1); }
static char *read_log(uint32_t run) {
    char *full=calloc(1,1);size_t offset=0;
    for(;;) {
        cJSON *p=cJSON_CreateObject();cJSON_AddNumberToObject(p,"run_id",run);cJSON_AddNumberToObject(p,"offset",offset);
        ts_api_result_t r={0};assert(api_lpmu_access_log(p,&r)==ESP_OK);cJSON_Delete(p);
        char *json=cJSON_PrintUnformatted(r.data);assert(strlen(json)<7400);free(json);
        const char *part=cJSON_GetObjectItem(r.data,"output")->valuestring;
        size_t next=cJSON_GetObjectItem(r.data,"next_offset")->valuedouble;
        bool done=cJSON_IsTrue(cJSON_GetObjectItem(r.data,"done"));
        assert(strlen(part)<=LPMU_LOG_CHUNK && next==offset+strlen(part) && (done || next>offset));
        full=realloc(full,next+1);memcpy(full+offset,part,strlen(part)+1);offset=next;
        ts_api_result_free(&r);if(done)break;
    }
    return full;
}
static void check_case(const char *out,const char *err,int exit,int transport,bool success,size_t chunk) {
    script_stdout=out;script_stderr=err;script_exit=exit;script_error=transport;output_chunk=chunk;
    int before=secret_wipes;start_run();finish_run();assert(secret_wipes==before+1);
    assert(s_status.stage==(success?LPMU_STAGE_SUCCESS:LPMU_STAGE_FAILED));
    ts_api_result_t r={0};assert(api_lpmu_access_status(NULL,&r)==ESP_OK);
    char *json=cJSON_PrintUnformatted(r.data);assert(!strstr(json,secret));free(json);ts_api_result_free(&r);
    char *log=read_log(s_status.run_id);
    assert(!strstr(log,secret));
    if(success)assert(strstr(log,"sudo -S -k -p '' -- ./setup-smart-route.sh"));
    if(out)assert(strstr(log,out));if(err)assert(strstr(log,err));free(log);
}
static void invalid_case(cJSON *p,esp_err_t expected) {
    int before=task_starts;ts_api_result_t r={0};assert(api_lpmu_access_start(p,&r)==expected);
    assert(task_starts==before && !secret_buffer);ts_api_result_free(&r);cJSON_Delete(p);
}
int main(void) {
    const char *both="✓ 互联网连接正常！\n=== 配置完成 ===\n";
    invalid_case(NULL,ESP_ERR_INVALID_ARG);invalid_case(cJSON_Parse("{}"),ESP_ERR_INVALID_ARG);
    invalid_case(cJSON_Parse("{\"sudo_password\":\"\"}"),ESP_ERR_INVALID_ARG);
    invalid_case(cJSON_Parse("{\"sudo_password\":\"first\\nsecond\"}"),ESP_ERR_INVALID_ARG);
    configured=false;invalid_case(cJSON_Parse("{\"sudo_password\":\"test\"}"),ESP_ERR_INVALID_STATE);configured=true;
    mux_target=0;invalid_case(cJSON_Parse("{\"sudo_password\":\"test\"}"),ESP_ERR_INVALID_STATE);mux_target=TS_USB_MUX_LPMU;
    allocation_failure=true;invalid_case(cJSON_Parse("{\"sudo_password\":\"test\"}"),ESP_ERR_NO_MEM);allocation_failure=false;
    task_failure=true;ts_api_result_t r={0};cJSON *p=cJSON_Parse("{\"sudo_password\":\"test\"}");
    int wipes=secret_wipes;assert(api_lpmu_access_start(p,&r)==ESP_ERR_NO_MEM);assert(secret_wipes==wipes+1 && !secret_buffer);
    assert(s_status.stage==LPMU_STAGE_FAILED && !s_status.running && !s_lpmu_task && (intptr_t)s_run_log==-1);
    assert(s_status.stack_min_free_bytes==UINT32_MAX);
    ts_api_result_free(&r);cJSON_Delete(p);task_failure=false;
    check_case(both,NULL,0,ESP_OK,true,1); // split every UTF-8 byte
    assert(uploads==0); // Existing project must not be replaced.
    check_case("=== 配置完成 ===\n",NULL,0,ESP_OK,false,512);
    check_case("✓ 互联网连接正常！\n",NULL,0,ESP_OK,false,512);
    check_case("ordinary successful exit\n",NULL,0,ESP_OK,false,512);
    check_case("ordinary stdout\n",both,0,ESP_OK,false,512); // stderr cannot confirm stdout
    check_case(both,NULL,1,ESP_OK,false,512);
    check_case(both,NULL,0,ESP_ERR_TIMEOUT,false,512);
    check_case(NULL,"Sorry, try again.\nsudo: no password was provided\n",1,ESP_OK,false,512);
    assert(strstr(s_status.output_tail,"Sorry, try again."));
    char long_output[4096];snprintf(long_output,sizeof(long_output),"✓ 互联网连接正常！\n");
    size_t start=strlen(long_output);memset(long_output+start,'x',3000);
    strcpy(long_output+start+3000,"\n=== 配置完成 ===\n");
    check_case(long_output,NULL,0,ESP_OK,true,4096);assert(!strstr(s_status.output_tail,"互联网连接正常"));
    cJSON *old=cJSON_CreateObject();cJSON_AddNumberToObject(old,"run_id",s_status.run_id);
    char *huge=malloc(100*1024+1);
    size_t complete_length=100*1024/strlen("网络连接记录\n")*strlen("网络连接记录\n");
    for(size_t i=0;i<complete_length;i++)huge[i]="网络连接记录\n"[i%strlen("网络连接记录\n")];
    huge[complete_length]=0;
    check_case(huge,NULL,0,ESP_OK,false,4096);free(huge);
    assert(s_status.log_len>100*1024);
    r=(ts_api_result_t){0};assert(api_lpmu_access_log(old,&r)==ESP_ERR_NOT_FOUND);ts_api_result_free(&r);cJSON_Delete(old);
    missing_project=true;check_case(both,NULL,0,ESP_OK,true,512);assert(uploads==1);missing_project=false;
    for(int failure=0;failure<3;failure++) {
        missing_project=failure!=2;upload_error=failure==0?ESP_FAIL:0;remote_extract_exit=failure==1?1:0;remote_chmod_exit=failure==2?1:0;
        int runs=script_runs;
        script_stdout=both;script_stderr=NULL;script_exit=script_error=0;start_run();finish_run();
        assert(s_status.stage==LPMU_STAGE_FAILED && script_runs==runs);
        assert(strstr(s_status.last_error,failure==0?"SCP upload":failure==1?"remote extract":"remote chmod"));
        missing_project=false;upload_error=remote_extract_exit=remote_chmod_exit=0;
    }
    connection_error=ESP_FAIL;int runs=script_runs;check_case(NULL,NULL,-1,ESP_OK,false,512);assert(script_runs==runs);connection_error=0;
    host_status=TS_HOST_VERIFY_MISMATCH;check_case(NULL,NULL,-1,ESP_OK,false,512);assert(script_runs==runs);host_status=TS_HOST_VERIFY_OK;
    host_status=TS_HOST_VERIFY_NOT_FOUND;check_case(NULL,NULL,-1,ESP_OK,false,512);assert(script_runs==runs);host_status=TS_HOST_VERIFY_OK;
    script_stdout=both;script_exit=script_error=0;start_run();
    p=cJSON_Parse("{\"sudo_password\":\"second\"}");r=(ts_api_result_t){0};
    assert(api_lpmu_access_start(p,&r)==ESP_ERR_INVALID_STATE && r.code==TS_API_ERR_BUSY);
    ts_api_result_free(&r);cJSON_Delete(p);
    p=cJSON_CreateObject();cJSON_AddNumberToObject(p,"run_id",s_status.run_id);
    assert(api_lpmu_access_log(p,&r)==ESP_ERR_INVALID_STATE);ts_api_result_free(&r);cJSON_Delete(p);finish_run();
    p=cJSON_CreateObject();cJSON_AddNumberToObject(p,"run_id",s_status.run_id);cJSON_AddNumberToObject(p,"offset",-1);
    assert(api_lpmu_access_log(p,&r)==ESP_ERR_INVALID_ARG);ts_api_result_free(&r);cJSON_Delete(p);
    p=cJSON_CreateObject();cJSON_AddNumberToObject(p,"run_id",s_status.run_id);
    log_read_allocation_failure=true;assert(api_lpmu_access_log(p,&r)==ESP_ERR_NO_MEM);log_read_allocation_failure=false;
    ts_api_result_free(&r);cJSON_Delete(p);
    for(int failure=0;failure<4;failure++) {
        p=cJSON_CreateObject();cJSON_AddNumberToObject(p,"run_id",s_status.run_id);
        log_open_failure=failure==0;log_read_failure=failure==1;log_short_read=failure==2;log_seek_failure=failure==3;
        assert(api_lpmu_access_log(p,&r)==ESP_FAIL);ts_api_result_free(&r);cJSON_Delete(p);
        log_open_failure=log_read_failure=log_short_read=log_seek_failure=false;
    }
    FILE *edited=fopen(LPMU_LOG_PATH,"wb");assert(edited);fputc(0xe4,edited);fclose(edited);
    p=cJSON_CreateObject();cJSON_AddNumberToObject(p,"run_id",s_status.run_id);
    assert(api_lpmu_access_log(p,&r)==ESP_FAIL);ts_api_result_free(&r);cJSON_Delete(p);
    // Even when metadata matches, an incomplete final UTF-8 character is an error.
    s_status.log_len=1;
    p=cJSON_CreateObject();cJSON_AddNumberToObject(p,"run_id",s_status.run_id);
    assert(api_lpmu_access_log(p,&r)==ESP_FAIL);ts_api_result_free(&r);cJSON_Delete(p);
    assert(unlink(LPMU_LOG_PATH)==0);
    p=cJSON_CreateObject();cJSON_AddNumberToObject(p,"run_id",s_status.run_id);
    assert(api_lpmu_access_log(p,&r)==ESP_FAIL);ts_api_result_free(&r);cJSON_Delete(p);
    script_stdout=both;script_stderr=NULL;start_run();finish_run();
    p=cJSON_CreateObject();cJSON_AddNumberToObject(p,"run_id",s_status.run_id);
    replace_run_during_read=true;
    assert(api_lpmu_access_log(p,&r)==ESP_ERR_NOT_FOUND);ts_api_result_free(&r);cJSON_Delete(p);
    assert(!s_status.running && !secret_buffer); // Replacement really rewrote the log.
    for(int failure=0;failure<3;failure++) {
        log_open_failure=failure==0;log_write_failure=failure==1;log_short_write=failure==2;
        start_run();finish_run();assert(s_status.stage==LPMU_STAGE_SUCCESS && s_status.log_error!=ESP_OK);
        log_open_failure=log_write_failure=log_short_write=false;
    }
    log_close_failure=true;start_run();finish_run();log_close_failure=false;
    assert(s_status.stage==LPMU_STAGE_SUCCESS && s_status.log_error==ESP_FAIL);
    p=cJSON_CreateObject();cJSON_AddNumberToObject(p,"run_id",s_status.run_id);
    assert(api_lpmu_access_log(p,&r)==ESP_FAIL);ts_api_result_free(&r);cJSON_Delete(p);
    sd_failure=true;start_run();finish_run();assert(s_status.stage==LPMU_STAGE_SUCCESS && s_status.log_error==ESP_FAIL);
    p=cJSON_CreateObject();cJSON_AddNumberToObject(p,"run_id",s_status.run_id);
    assert(api_lpmu_access_log(p,&r)==ESP_FAIL);ts_api_result_free(&r);cJSON_Delete(p);
    assert(cleanup_observations>5 && log_close_observations>5 && io_observations>20 && log_read_calls>5);
    r=(ts_api_result_t){0};assert(api_lpmu_access_status(NULL,&r)==ESP_OK);
    assert(!cJSON_GetObjectItem(r.data,"diagnostics"));ts_api_result_free(&r);
    sd_failure=false;script_stdout=both;script_exit=script_error=0;start_run();finish_run();
    char *measured_log=read_log(s_status.run_id);free(measured_log);
    p=cJSON_Parse("{\"diagnostics\":1}");r=(ts_api_result_t){0};assert(api_lpmu_access_status(p,&r)==ESP_OK);
    cJSON *diag=cJSON_GetObjectItem(r.data,"diagnostics");assert(diag);
    assert(cJSON_GetObjectItem(diag,"stack_min_free_bytes")->valuedouble==1536);
    assert(cJSON_GetObjectItem(diag,"stack_allocated_bytes")->valuedouble==8192);
    assert(cJSON_GetObjectItem(diag,"log_init_us")->valuedouble>0);
    assert(cJSON_GetObjectItem(diag,"log_write_count")->valuedouble>0 && cJSON_GetObjectItem(diag,"log_read_count")->valuedouble>0);
    assert(cJSON_GetObjectItem(diag,"log_write_us")->valuedouble>=cJSON_GetObjectItem(diag,"log_write_max_us")->valuedouble);
    assert(cJSON_GetObjectItem(diag,"log_read_us")->valuedouble>=cJSON_GetObjectItem(diag,"log_read_max_us")->valuedouble);
    ts_api_result_free(&r);cJSON_Delete(p);
    pthread_mutex_destroy(&mutex);
    puts("PASS actual LPMU task/API: task-owned SD initialization, lock-free file I/O, 1 KiB POSIX reads, truncation/incomplete UTF-8/read/seek/write/close failures, replaced-run rejection, cleanup-before-terminal publication, password stdin/wipe, upload, host gate, split markers, zero-exit unconfirmed, 100 KiB exact reconstruction and PSRAM-only read allocation failure");
}
'''
with tempfile.TemporaryDirectory(prefix='ts-lpmu-test-') as tmp:
    path = Path(tmp)
    (path / 'test.c').write_text(code.replace('TEST_LOG_PATH',str(path / 'execution.log')))
    command = ['cc', '-std=c11', '-D_DEFAULT_SOURCE', '-g', '-fsanitize=address,undefined', '-Wno-deprecated-declarations',
               '-I' + str(root / 'tests/certificate/stubs'),
               '-I' + str(root / 'components/ts_api/include'),
               '-I' + str(root / 'components/ts_security/include'),
               '-I' + str(idf / 'components/json/cJSON'),
               str(path / 'test.c'), str(idf / 'components/json/cJSON/cJSON.c'),
               '-lpthread', '-o', str(path / 'test')]
    subprocess.run(command, check=True)
    subprocess.run([str(path / 'test')], check=True)
