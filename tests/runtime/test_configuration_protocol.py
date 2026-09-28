"""Production mutation wrappers + service protocol, mocked storage and transport."""
from pathlib import Path
import re, subprocess, os
build=Path('/tmp/tianshan-runtime-tests'); build.mkdir(exist_ok=True)
def extract(path,name):
 s=Path(path).read_text();m=re.search(r'^(?:static )?[^\n;]+\b'+name+r'\([^;]+?\)\s*\{',s,re.M);assert m
 return s[m.start():s.index('\n}',m.start())+2]
s=Path('tests/runtime/test_service_watch.c').read_text().split('int main(void)')[0]
s=s.replace('static int fail_create;', '''static char configured_host[64]="192.0.2.8", connected_host[64];
static int interleave, writer_result; static atomic_int writer_entered;
static pthread_t writer; static void host_interleave(void);
static int fail_create;''')
s=s.replace('strcpy(out->host,"192.0.2.8");','strcpy(out->host,configured_host);')
s=s.replace('out->port=22;return ESP_OK;}', 'out->port=22;if(interleave)host_interleave();return ESP_OK;}')
s=s.replace('(*out)->config=*cfg;++ssh_live;', '(*out)->config=*cfg;strcpy(connected_host,cfg->host);++ssh_live;')
s+='''
static esp_err_t host_add_impl(const ts_ssh_host_config_t *cfg) {strcpy(configured_host,cfg->host);return ESP_OK;}
static esp_err_t command_add_impl(const ts_ssh_command_config_t *cfg,char*out,size_t len) {command=*cfg;return ESP_OK;}
static int storage_fail;
static esp_err_t command_remove_impl(const char *id) {uint32_t pin;assert(!ts_ssh_service_start_admissible(id));assert(ts_ssh_service_pin(&command,"192.0.2.8",22,&pin)==ESP_ERR_INVALID_STATE);if(storage_fail)return ESP_FAIL;command.id[0]=0;return ESP_OK;}
'''
s+=extract('components/ts_security/src/ts_ssh_hosts_config.c','ts_ssh_hosts_config_add')+'\n'
for name in ['ts_ssh_commands_config_add','ts_ssh_commands_config_remove']:
 s+=extract('components/ts_security/src/ts_ssh_commands_config.c',name)+'\n'
s+='''
static void *edit_host(void *unused) {
 ts_ssh_host_config_t cfg={0};strcpy(cfg.id,"host");strcpy(cfg.host,"192.0.2.9");strcpy(cfg.keyid,"synthetic-key");strcpy(cfg.username,"test");cfg.port=22;
 atomic_store(&writer_entered,1);writer_result=ts_ssh_hosts_config_add(&cfg);return NULL;
}
static void host_interleave(void) {
 interleave=0;assert(!pthread_create(&writer,NULL,edit_host,NULL));
 while(!atomic_load(&writer_entered)) { struct timespec t={0,100000};nanosleep(&t,NULL); }
}
int main(void) {
 strcpy(command.id,"model");strcpy(command.name,"model");strcpy(command.host_id,"host");strcpy(command.var_name,"model");command.nohup=command.service_mode=command.enabled=true;
 assert(ts_ssh_service_init()==ESP_OK);assert(ts_ssh_log_watch_init()==ESP_OK);
 ts_ssh_service_status_t st;assert(ts_ssh_service_query(command.id,&st)==ESP_OK);
 remote_running=1;interleave=1;
 assert(ts_ssh_service_query(command.id,&st)==ESP_OK);pthread_join(writer,NULL);
 assert(writer_result==ESP_ERR_INVALID_STATE&&!strcmp(connected_host,"192.0.2.8"));
 assert(ts_ssh_service_stop(command.id,&st)==ESP_OK);
 storage_fail=1;assert(ts_ssh_commands_config_remove("model")==ESP_FAIL);assert(ts_ssh_service_query("model",&st)==ESP_OK);
 storage_fail=0;assert(ts_ssh_commands_config_remove("model")==ESP_OK);
 ts_ssh_command_config_t replacement=command;strcpy(replacement.id,"new-model");
 assert(ts_ssh_commands_config_add(&replacement,NULL,0)==ESP_OK);
 assert(ts_ssh_service_query("new-model",&st)==ESP_OK);
 puts("PASS actual configuration wrappers: host edit vs query, immutable target, failed delete preserves registration, successful delete then same-name replacement");
}
'''
(build/'configuration_protocol.c').write_text(s)
env={**os.environ,'DEVELOPER_DIR':'/Library/Developer/CommandLineTools'}
subprocess.run(['cc','-std=c11','-g','-fsanitize=address,undefined','-isysroot','/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk',*[f'-I{x}' for x in ['tests/runtime/state_stubs','tests/runtime/ssh_stubs','tests/runtime/stubs','tests/certificate/stubs','components/ts_security/include','components/ts_automation/include']],str(build/'configuration_protocol.c'),'components/ts_security/src/ts_ssh_service.c','components/ts_security/src/ts_ssh_log_watch.c','components/ts_security/src/ts_ssh_probe.c','-lpthread','-o',str(build/'configuration_protocol')],check=True,env=env)
subprocess.run([str(build/'configuration_protocol')],check=True,env=env)
