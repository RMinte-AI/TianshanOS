"""Execute production host storage/import and service guards with fake NVS/SD/SSH."""
from pathlib import Path
import os
import re
import subprocess

BUILD = Path('/tmp/tianshan-ssh-host-tests')
BUILD.mkdir(exist_ok=True)
IDF = Path(os.environ.get('IDF_PATH', '/Users/massif/esp/v5.5.2/esp-idf'))
SOURCE = Path('components/ts_security/src/ts_ssh_hosts_config.c').read_text()

def extract(name):
    match = re.search(r'^(?:static )?[^\n;]+\b' + name + r'\([^;]+?\)\s*\{', SOURCE, re.M)
    assert match, name
    return SOURCE[match.start():SOURCE.index('\n}', match.start()) + 2] + '\n'

prefix = Path('tests/runtime/test_service_watch.c').read_text().split('int main(void)')[0]
prefix = '\n'.join(line for line in prefix.splitlines()
                   if not line.startswith('esp_err_t ts_ssh_hosts_config_get('))
prefix = prefix.replace('return ts_ssh_commands_config_iterate(cb,arg,offset,limit,total);',
                       'return strcmp(host,command.host_id) ? ESP_OK : ts_ssh_commands_config_iterate(cb,arg,offset,limit,total);')
entry = re.search(r'typedef struct __attribute__\(\(packed\)\) \{.*?\} nvs_host_entry_t;', SOURCE, re.S).group()
state = re.search(r'static struct \{.*?\} s_state = \{0\};', SOURCE, re.S).group()
code = prefix + '\n#include "freertos/semphr.h"\n#include "cJSON.h"\n#include <dirent.h>\n#include <sys/stat.h>\n#include <unistd.h>\n' + entry + '\n' + state
code += r'''
#include "ts_rule_engine.h"
esp_err_t ts_rule_dependency_change(ts_rule_dependency_t kind,const char *id,const void *next){return ESP_OK;}
#define NVS_KEY_PREFIX "h_"
static char sd_dir[256];
#undef TS_SSH_HOSTS_SDCARD_DIR
#define TS_SSH_HOSTS_SDCARD_DIR sd_dir
static nvs_host_entry_t rows[TS_SSH_HOSTS_MAX];
static bool present[TS_SSH_HOSTS_MAX];
static int read_error, write_error, commit_error, nvs_writes, syncs;
static atomic_int writer_started;
static int writer_result, clear_calls, export_calls;
static bool normal_add_during_import;
static bool pause_import, import_entered, resume_import;
static pthread_mutex_t import_barrier=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t import_condition=PTHREAD_COND_INITIALIZER;
static atomic_int backup_started, backup_writes;
static int import_result;
static ts_ssh_host_config_t concurrent_host;
static TaskHandle_t initial_loader;
static atomic_int initial_load_result=ESP_ERR_INVALID_STATE;
static bool s_hosts_pending_export;
static int slot(const char *key) { int n=-1; assert(sscanf(key,"h_%d",&n)==1 && n>=0 && n<TS_SSH_HOSTS_MAX); return n; }
esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *out,size_t *len) {
    if(read_error) return read_error;
    int n=slot(key); if(!present[n]) return ESP_ERR_NVS_NOT_FOUND;
    assert(*len>=sizeof(rows[n])); memcpy(out,&rows[n],sizeof(rows[n])); *len=sizeof(rows[n]); return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *in,size_t len) {
    ++nvs_writes; if(write_error) return write_error;
    int n=slot(key); assert(len==sizeof(rows[n])); memcpy(&rows[n],in,len); present[n]=true; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { return commit_error; }
void ts_ssh_hosts_config_sync_to_sdcard(void) { ++syncs; }
static bool is_sdcard_mounted(void) { return true; }
static esp_err_t host_add_guarded(const ts_ssh_host_config_t *,bool);
static TaskHandle_t xTaskGetCurrentTaskHandle(void) { return self; }
static void vTaskDelay(unsigned ticks) {}
esp_err_t ts_ssh_hosts_config_clear(void) { ++clear_calls; memset(present,0,sizeof(present)); return ESP_OK; }
static esp_err_t ensure_hosts_dir(void) {return ESP_OK;}
static cJSON *host_to_json(const ts_ssh_host_config_t *cfg);
/* File/crypto seam only; exporter/iterator and importer execute production code. */
static esp_err_t export_host_to_file(const ts_ssh_host_config_t *cfg) {
    char path[512];snprintf(path,sizeof(path),"%s/%s.json",sd_dir,cfg->id);
    cJSON *obj=host_to_json(cfg);char *json=cJSON_PrintUnformatted(obj);assert(json);
    FILE *f=fopen(path,"wb");assert(f);fwrite(json,1,strlen(json),f);fclose(f);
    cJSON_Delete(obj);free(json);atomic_fetch_add(&backup_writes,1);++export_calls;return ESP_OK;
}
int ts_ssh_hosts_config_count(void) { int n=0; for(int i=0;i<TS_SSH_HOSTS_MAX;i++) n+=present[i]; return n; }
static esp_err_t ts_config_pack_load_with_priority(const char *path,char **out,size_t *len,bool *encrypted) {
    if(normal_add_during_import) {
        normal_add_during_import=false;
        assert(ts_ssh_hosts_config_add(&concurrent_host)==ESP_OK);
    }
    if(pause_import) {
        pthread_mutex_lock(&import_barrier);import_entered=true;pthread_cond_broadcast(&import_condition);
        while(!resume_import)pthread_cond_wait(&import_condition,&import_barrier);
        pthread_mutex_unlock(&import_barrier);
    }
    char name[256]; snprintf(name,sizeof(name),"%.*s.tscfg",(int)strlen(path)-5,path);
    FILE *f=fopen(name,"rb"); *encrypted=f!=NULL;
    if(!f) f=fopen(path,"rb");
    if(!f) return ESP_ERR_NOT_FOUND;
    fseek(f,0,SEEK_END); long size=ftell(f); rewind(f);
    *out=calloc(size+1,1); assert(*out); *len=fread(*out,1,size,f); fclose(f);
    if(!strcmp(*out,"DECRYPT_FAIL")) {free(*out);*out=NULL;return ESP_FAIL;}
    return ESP_OK;
}
'''
for name in ['make_nvs_key', 'get_current_time', 'host_add_impl', 'ts_ssh_hosts_config_get',
             'host_add_guarded', 'ts_ssh_hosts_config_add', 'json_to_host', 'load_hosts_from_dir',
             'ts_ssh_hosts_config_import_from_sdcard', 'host_entry_to_config',
             'ts_ssh_hosts_config_iterate', 'host_to_json', 'host_export_iterator_cb',
             'ts_ssh_hosts_config_export_to_sdcard', 'hosts_load_once', 'hosts_deferred_export_task']:
    code += extract(name)
code += r'''
static ts_ssh_host_config_t host(const char *id) {
    ts_ssh_host_config_t cfg={.port=22,.enabled=true};
    snprintf(cfg.id,sizeof(cfg.id),"%s",id); strcpy(cfg.host,"192.0.2.8");
    strcpy(cfg.username,"test"); strcpy(cfg.keyid,"synthetic-key"); return cfg;
}
static void reset_store(void) {
    memset(present,0,sizeof(present));read_error=write_error=commit_error=nvs_writes=syncs=0;
}
static void put(const char *filename,const char *content) {
    char path[512];snprintf(path,sizeof(path),"%s/%s",sd_dir,filename);
    FILE *f=fopen(path,"wb");assert(f);assert(fwrite(content,1,strlen(content),f)==strlen(content));fclose(f);
}
static void clean_files(void) {
    DIR *dir=opendir(sd_dir);assert(dir);struct dirent *e;
    while((e=readdir(dir))) if(e->d_name[0]!='.') {char path[512];snprintf(path,sizeof(path),"%s/%s",sd_dir,e->d_name);assert(!unlink(path));}
    closedir(dir);
}
static void stopped_registry(void) {
    uint32_t pin; assert(ts_ssh_service_pin(&command,"192.0.2.8",22,&pin)==ESP_OK);
    ts_ssh_service_unpin(command.id,pin);
    ts_ssh_service_status_t status;remote_running=0;
    assert(ts_ssh_service_query(command.id,&status)==ESP_OK && !strcmp(status.state,"stopped"));
    ts_ssh_binding_lock();ts_ssh_service_forget_stopped();ts_ssh_binding_unlock();
}
static void *add_writer(void *arg) {
    atomic_store(&writer_started,1);writer_result=ts_ssh_hosts_config_add(arg);return NULL;
}
static void *boot_loader(void *unused) { self=(TaskHandle_t)&concurrent_host;hosts_deferred_export_task(NULL);return NULL; }
static void *import_worker(void *arg) {import_result=ts_ssh_hosts_config_import_from_sdcard(true);return NULL;}
static void *backup_worker(void *arg) {
    atomic_store(&backup_started,1);assert(ts_ssh_hosts_config_export_to_sdcard()==ESP_OK);return NULL;
}
static bool accept_host(const ts_ssh_host_config_t *cfg,size_t index,void *context) {return true;}
int main(int argc,char **argv) {
    assert(argc==2);snprintf(sd_dir,sizeof(sd_dir),"%s",argv[1]);
    s_state.initialized=true;s_state.mutex=xSemaphoreCreateMutex();s_state.sd_mutex=xSemaphoreCreateMutex();
    ts_ssh_host_config_t cfg=host("host"),out;
    assert(ts_ssh_service_host_runtime_protected(cfg.id));
    assert(ts_ssh_hosts_config_add(&cfg)==ESP_ERR_INVALID_STATE && nvs_writes==0);
    assert(ts_ssh_service_init()==ESP_OK);assert(ts_ssh_log_watch_init()==ESP_OK);
    strcpy(command.id,"model");strcpy(command.host_id,cfg.id);strcpy(command.name,"model");
    command.nohup=command.service_mode=true;
    assert(ts_ssh_service_host_protected(cfg.id)); /* historical reference remains conservatively protected */
    assert(!ts_ssh_service_host_runtime_protected(cfg.id));
    assert(ts_ssh_hosts_config_add(&cfg)==ESP_OK && syncs==1);
    assert(ts_ssh_hosts_config_get(cfg.id,&out)==ESP_OK && !strcmp(out.keyid,cfg.keyid));
    strcpy(cfg.keyid,"replacement");
    assert(ts_ssh_hosts_config_add(&cfg)==ESP_ERR_INVALID_STATE);
    strcpy(cfg.keyid,"synthetic-key");
    stopped_registry();reset_store();
    uint32_t pin;assert(ts_ssh_service_pin(&command,cfg.host,22,&pin)==ESP_OK);
    assert(ts_ssh_hosts_config_add(&cfg)==ESP_ERR_INVALID_STATE && nvs_writes==0);
    ts_ssh_service_unpin(command.id,pin);
    assert(ts_ssh_service_host_runtime_protected(cfg.id)); /* unknown is not stopped */
    assert(host_add_impl(&cfg,false)==ESP_OK);stopped_registry();reset_store();
    pthread_t writer;atomic_store(&writer_started,0);
    ts_ssh_binding_lock();assert(!pthread_create(&writer,NULL,add_writer,&cfg));
    while(!atomic_load(&writer_started)) {struct timespec t={0,100000};nanosleep(&t,NULL);}
    assert(ts_ssh_service_pin(&command,cfg.host,22,&pin)==ESP_OK);ts_ssh_binding_unlock();
    pthread_join(writer,NULL);assert(writer_result==ESP_ERR_INVALID_STATE && nvs_writes==0);
    ts_ssh_service_unpin(command.id,pin);assert(host_add_impl(&cfg,false)==ESP_OK);stopped_registry();reset_store();
    read_error=ESP_FAIL;assert(ts_ssh_hosts_config_add(&cfg)==ESP_FAIL && nvs_writes==0);read_error=0;
    write_error=ESP_FAIL;assert(ts_ssh_hosts_config_add(&cfg)==ESP_FAIL && syncs==0);write_error=0;
    commit_error=ESP_FAIL;assert(ts_ssh_hosts_config_add(&cfg)==ESP_FAIL && syncs==0);reset_store();
    for(int i=0;i<TS_SSH_HOSTS_MAX;i++) {char id[32];snprintf(id,sizeof(id),"host-%d",i);ts_ssh_host_config_t c=host(id);assert(host_add_impl(&c,false)==ESP_OK);}
    assert(ts_ssh_hosts_config_add(&cfg)==ESP_ERR_NO_MEM);reset_store();
    ts_ssh_host_config_t keep=host("keep");assert(ts_ssh_hosts_config_add(&keep)==ESP_OK);syncs=0;
    put("host.json","{\"id\":\"host\",\"host\":\"192.0.2.99\",\"username\":\"stale\"}");
    put("host.tscfg","{\"id\":\"host\",\"host\":\"192.0.2.8\",\"username\":\"test\",\"keyid\":\"synthetic-key\"}");
    concurrent_host=host("concurrent");normal_add_during_import=true;
    assert(ts_ssh_hosts_config_import_from_sdcard(true)==ESP_OK);
    assert(ts_ssh_hosts_config_count()==3 && syncs==1 && clear_calls==0);
    assert(ts_ssh_hosts_config_get("host",&out)==ESP_OK && !strcmp(out.host,"192.0.2.8"));
    assert(ts_ssh_hosts_config_get("keep",&out)==ESP_OK);
    put("fresh.json","{\"id\":\"fresh\",\"host\":\"192.0.2.10\",\"username\":\"test\"}");
    put("broken.json","bad JSON");put("encrypted.tscfg","DECRYPT_FAIL");
    esp_err_t ret=ts_ssh_hosts_config_import_from_sdcard(true);
    assert(ret!=ESP_OK && ret!=ESP_ERR_NOT_FOUND && ts_ssh_hosts_config_count()==4);
    assert(ts_ssh_hosts_config_get("fresh",&out)==ESP_OK);
    clean_files();
    put("protected.json","{\"id\":\"host\",\"host\":\"192.0.2.9\",\"username\":\"test\"}");
    assert(ts_ssh_hosts_config_import_from_sdcard(true)==ESP_ERR_INVALID_STATE);
    assert(ts_ssh_hosts_config_get("host",&out)==ESP_OK && !strcmp(out.host,"192.0.2.8"));
    clean_files();assert(ts_ssh_hosts_config_import_from_sdcard(true)==ESP_ERR_NOT_FOUND);
    put("write-fail.json","{\"id\":\"failed\",\"host\":\"192.0.2.11\",\"username\":\"test\"}");
    write_error=ESP_FAIL;assert(ts_ssh_hosts_config_import_from_sdcard(true)==ESP_FAIL);write_error=0;
    assert(ts_ssh_hosts_config_count()==4 && ts_ssh_hosts_config_get("keep",&out)==ESP_OK);
    clean_files();
    put("host.tscfg","{\"id\":\"host\",\"host\":\"192.0.2.8\",\"username\":\"test\",\"keyid\":\"synthetic-key\"}");
    pthread_t boot;assert(!pthread_create(&boot,NULL,boot_loader,NULL));pthread_join(boot,NULL);
    assert(clear_calls==0 && ts_ssh_hosts_config_count()==4);
    clean_files();
    if(!getenv("REVIEW_CASE") || !strcmp(getenv("REVIEW_CASE"),"sd")) {
        put("keep.json","{\"id\":\"keep\",\"host\":\"192.0.2.42\",\"username\":\"test\",\"keyid\":\"synthetic-key\"}");
        concurrent_host=host("during-restore");normal_add_during_import=true;
        pause_import=true;import_entered=resume_import=false;atomic_store(&backup_writes,0);atomic_store(&backup_started,0);
        pthread_t reader,backup;assert(!pthread_create(&reader,NULL,import_worker,NULL));
        pthread_mutex_lock(&import_barrier);
        while(!import_entered)pthread_cond_wait(&import_condition,&import_barrier);
        pthread_mutex_unlock(&import_barrier);
        assert(!pthread_create(&backup,NULL,backup_worker,NULL));
        while(!atomic_load(&backup_started)) {struct timespec t={0,1000000};nanosleep(&t,NULL);}
        struct timespec wait={0,50000000};nanosleep(&wait,NULL);
        /* Resume the actual read after the queued backup has had a chance to run. */
        pthread_mutex_lock(&import_barrier);resume_import=true;pthread_cond_broadcast(&import_condition);pthread_mutex_unlock(&import_barrier);
        pthread_join(reader,NULL);pthread_join(backup,NULL);pause_import=false;
        assert(import_result==ESP_OK && ts_ssh_hosts_config_get("keep",&out)==ESP_OK);
        assert(!strcmp(out.host,"192.0.2.42")); /* Backup must not overwrite unread SD input. */
        assert(atomic_load(&backup_writes)>0);clean_files();
    }
    if(!getenv("REVIEW_CASE") || !strcmp(getenv("REVIEW_CASE"),"list")) {
        size_t total=0;read_error=ESP_FAIL;
        assert(ts_ssh_hosts_config_iterate(accept_host,NULL,0,0,&total)==ESP_FAIL);
        read_error=0;assert(ts_ssh_hosts_config_iterate(accept_host,NULL,0,0,&total)==ESP_OK && total>0);
    }
    puts("PASS production host NVS/import + service registry: orphan recovery, uninitialized/pinned/unknown guards, atomic admission, storage errors/full capacity, SD precedence/merge/failures, concurrent add backup, boot retains unrelated hosts, queued backup preserves SD input, list storage error propagation");
}
'''
file = BUILD / 'ssh_hosts.c'
file.write_text(code)
includes = ['tests/runtime/state_stubs', 'tests/runtime/ssh_stubs', 'tests/runtime/stubs',
            'tests/certificate/stubs', 'components/ts_security/include', 'components/ts_automation/include',
            str(IDF / 'components/json/cJSON')]
env = {**os.environ, 'DEVELOPER_DIR': '/Library/Developer/CommandLineTools'}
subprocess.run(['cc', '-std=c11', '-g', '-Wno-deprecated-declarations', '-fsanitize=address,undefined',
                *[f'-I{x}' for x in includes], str(file),
                'components/ts_security/src/ts_ssh_service.c',
                'components/ts_security/src/ts_ssh_log_watch.c',
                'components/ts_security/src/ts_ssh_probe.c',
                str(IDF / 'components/json/cJSON/cJSON.c'), '-lpthread', '-lm',
                '-o', str(BUILD / 'ssh_hosts')], check=True, env=env)
import tempfile
with tempfile.TemporaryDirectory(prefix='sd-', dir=BUILD) as directory:
    subprocess.run([str(BUILD / 'ssh_hosts'), directory], check=True,
                   env={**env, 'ASAN_OPTIONS': 'detect_leaks=0'})
