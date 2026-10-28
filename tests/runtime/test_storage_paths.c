/* Exercise production storage mutations and storage APIs against real temp files.
 * Only the mounted VFS, FAT name lookup, API result and driver boundaries are replaced. */
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ts_storage.h"
#include "ts_api.h"
#include "ts_config_pack.h"
#include "freertos/semphr.h"

static char root[] = "/private/tmp/tianshan-storage-paths-XXXXXX";
static unsigned mutations;
static pthread_mutex_t gate;
static const char *mapped(const char *path)
{
    static char paths[4][1024];
    static unsigned next;
    char *out = paths[next++ % 4];
    snprintf(out, 1024, "%s%s", root, path);
    return out;
}
static FILE *test_open(const char *path, const char *mode)
{
    if (strchr(mode, 'w') || strchr(mode, 'a')) ++mutations;
    return fopen(mapped(path), mode);
}
static int test_stat(const char *path, struct stat *out) { return stat(mapped(path), out); }
static int test_mkdir(const char *path, mode_t mode) { ++mutations;return mkdir(mapped(path), mode); }
static int test_unlink(const char *path) { ++mutations;return unlink(mapped(path)); }
static int test_rmdir(const char *path) { ++mutations;return rmdir(mapped(path)); }
static int test_rename(const char *from, const char *to) { ++mutations;return rename(mapped(from), mapped(to)); }
static DIR *test_opendir(const char *path) { return opendir(mapped(path)); }

#define fopen(path, mode) test_open(path, mode)
#define stat(path, out) test_stat(path, out)
#define mkdir(path, mode) test_mkdir(path, mode)
#define unlink(path) test_unlink(path)
#define rmdir(path) test_rmdir(path)
#define rename(from, to) test_rename(from, to)
#define opendir(path) test_opendir(path)
#include "../../components/ts_storage/src/ts_storage.c"
#include "../../components/ts_storage/src/ts_storage_paths.c"
#undef TAG
#include "../../components/ts_api/src/ts_api_storage.c"
#undef fopen
#undef stat
#undef mkdir
#undef unlink
#undef rmdir
#undef rename
#undef opendir

void ts_storage_sd_lock(void) { assert(!pthread_mutex_lock(&gate)); }
void ts_storage_sd_unlock(void) { assert(!pthread_mutex_unlock(&gate)); }
bool ts_storage_sd_name_is(const char *path, const char *name)
{
    /* Boundary models names actually resolved by FatFs. No guessed '~1' policy. */
    if (!strcmp(name, "config")) return !strcmp(path, "/sdcard/confıg");
    return !strcmp(name, "automation") &&
        (!strcmp(path, "/sdcard/config/AUTOMA~1") || !strcmp(path, "/sdcard/config/automatıon"));
}
esp_err_t ts_storage_unmount_auto(void) { return ESP_OK; }
esp_err_t ts_storage_unmount_spiffs(void) { return ESP_OK; }
esp_err_t ts_storage_unmount_sd(void) { return ESP_OK; }
esp_err_t ts_storage_mount_sd(const ts_sd_config_t *config) { (void)config;return ESP_OK; }
bool ts_storage_spiffs_mounted(void) { return false; }
bool ts_storage_sd_mounted(void) { return true; }
void ts_api_result_ok(ts_api_result_t *result, cJSON *data) { result->code=TS_API_OK;result->data=data; }
void ts_api_result_error(ts_api_result_t *result, ts_api_result_code_t code, const char *message)
{ result->code=code;result->message=(char *)message; }
esp_err_t ts_api_register_multiple(const ts_api_endpoint_t *endpoints, size_t count)
{ (void)endpoints;(void)count;return ESP_OK; }

typedef struct {const void *body;size_t body_len;} ts_http_request_t;
static const char *upload_path;
static int upload_status;
static char upload_reply[512];
static esp_err_t ts_http_get_query_param(ts_http_request_t *req,const char *key,char *out,size_t length)
{ (void)req;assert(!strcmp(key,"path"));return snprintf(out,length,"%s",upload_path)<(int)length?ESP_OK:ESP_ERR_INVALID_SIZE; }
static esp_err_t ts_http_send_json(ts_http_request_t *req,int status,const char *body)
{ (void)req;upload_status=status;snprintf(upload_reply,sizeof(upload_reply),"%s",body);return ESP_OK; }
static esp_err_t ts_http_send_error(ts_http_request_t *req,int status,const char *reason)
{ return ts_http_send_json(req,status,reason); }
ts_config_pack_result_t ts_config_pack_verify(const char *path,ts_config_pack_sig_info_t *info)
{ (void)path;(void)info;assert(!"configuration upload must be rejected before pack verification");return TS_CONFIG_PACK_ERR_IO; }
const char *ts_config_pack_strerror(ts_config_pack_result_t result) { (void)result;return "unused"; }
const char *ts_config_pack_reason(ts_config_pack_result_t result) { (void)result;return "unused"; }
static esp_err_t ts_ws_broadcast_to_topic(const char *topic,const cJSON *data)
{ (void)topic;(void)data;assert(!"no pack notification for a rejected upload");return ESP_FAIL; }
#include "storage_upload.inc"

static void upload(const char *path,int expected_status,const char *reason)
{
    upload_path=path;upload_status=0;upload_reply[0]=0;
    ts_http_request_t req={.body="uploaded",.body_len=8};
    assert(file_upload_handler(&req,NULL)==ESP_OK&&upload_status==expected_status);
    assert(strstr(upload_reply,reason));
}

static void original_file(const char *path, const char *text)
{
    FILE *file=test_open(path,"wb");assert(file);
    assert(fwrite(text,1,strlen(text),file)==strlen(text));assert(!fclose(file));
}
static void has_text(const char *path,const char *expected)
{
    char text[64];ssize_t size=ts_storage_read_file(path,text,sizeof(text));
    assert(size==(ssize_t)strlen(expected)&&!memcmp(text,expected,(size_t)size));
}
static void api_denied(ts_api_handler_t handler,const char *json)
{
    cJSON *params=cJSON_Parse(json);assert(params);ts_api_result_t result={0};
    unsigned before=mutations;
    assert(handler(params,&result)==ESP_ERR_NOT_ALLOWED);
    assert(result.code==TS_API_ERR_NO_PERMISSION&&!strcmp(result.message,"automation_configuration_managed"));
    assert(mutations==before);cJSON_Delete(params);cJSON_Delete(result.data);
}
int main(void)
{
    assert(mkdtemp(root));
    pthread_mutexattr_t attr;assert(!pthread_mutexattr_init(&attr));
    assert(!pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_RECURSIVE));
    assert(!pthread_mutex_init(&gate,&attr));assert(!pthread_mutexattr_destroy(&attr));
    assert(!test_mkdir("/sdcard",0755));assert(!test_mkdir("/sdcard/config",0755));
    assert(!test_mkdir("/sdcard/config/automation",0755));assert(!test_mkdir("/autocfg",0755));
    original_file("/sdcard/config/automation/state.json","current");
    original_file("/sdcard/config/ordinary.json","ordinary");
    original_file("/autocfg/state.json","emergency");

    const char *owned[]={
        "/autocfg", "/autocfg/state.pending", "/autocfg/../state.json",
        "/sdcard/config/automation", "/sdcard/config/automation/state.previous",
        "/sdcard/CONFIG/Automation/state.json", "/sdcard/config/automation. /state.json",
        "/sdcard/other/../config/./automation/state.json", "/sdcard/../config/automation/state.json",
        "/sdcard/config\\automation\\state.json", "/sdcard/config/AUTOMA~1/state.json",
        "/sdcard/confıg/automation/state.json", "/sdcard/config/automatıon/state.json"
    };
    for(size_t i=0;i<sizeof(owned)/sizeof(owned[0]);++i){
        assert(ts_storage_check_write_path(owned[i],false)==ESP_ERR_NOT_ALLOWED);
        unsigned before=mutations;
        assert(ts_storage_write_file(owned[i],"x",1)==ESP_ERR_NOT_ALLOWED);
        assert(ts_storage_append(owned[i],"x",1)==ESP_ERR_NOT_ALLOWED);
        assert(ts_storage_mkdir_p(owned[i])==ESP_ERR_NOT_ALLOWED);
        assert(ts_storage_copy("/sdcard/config/ordinary.json",owned[i])==ESP_ERR_NOT_ALLOWED);
        FILE *stream=(FILE *)1;
        assert(ts_storage_open_write(owned[i],&stream)==ESP_ERR_NOT_ALLOWED&&!stream);
        assert(mutations==before);
    }
    assert(ts_storage_check_write_path("/sdcard/config\t/not-config",true)==ESP_ERR_INVALID_ARG);
    assert(ts_storage_check_write_path("config/automation",true)==ESP_ERR_INVALID_ARG);
    assert(ts_storage_check_write_path("/spiffs/../config/automation/state.json",false)==ESP_OK);
    assert(ts_storage_check_write_path("/sdcard/config/AUTOMA~2/state.json",false)==ESP_OK);
    assert(ts_storage_check_write_path("/sdcard/config/automation-other/state.json",false)==ESP_OK);

    unsigned before=mutations;
    assert(ts_storage_delete("/sdcard/config/automation/state.json")==ESP_ERR_NOT_ALLOWED);
    assert(ts_storage_rmdir_r("/sdcard/config")==ESP_ERR_NOT_ALLOWED);
    assert(ts_storage_rmdir("/sdcard/config/automation")==ESP_ERR_NOT_ALLOWED);
    assert(ts_storage_rename("/sdcard/config","/sdcard/moved")==ESP_ERR_NOT_ALLOWED);
    assert(ts_storage_rename("/sdcard/config/ordinary.json","/sdcard/config/automation/state.json")==ESP_ERR_NOT_ALLOWED);
    assert(mutations==before);
    api_denied(api_storage_delete,"{\"path\":\"/sdcard/config\"}");
    api_denied(api_storage_rename,"{\"from\":\"/sdcard/config\",\"to\":\"/sdcard/moved\"}");
    api_denied(api_storage_rename,"{\"from\":\"/sdcard/config/ordinary.json\",\"to\":\"/autocfg/state.json\"}");
    api_denied(api_storage_mkdir,"{\"path\":\"/sdcard/config/automation/new\"}");
    before=mutations;
    upload("%2Fsdcard%2Fconfig%2Fautomation%2Fstate.json",409,"automation_configuration_managed");
    upload("/sdcard/config/AUTOMA~1/state.pending",409,"automation_configuration_managed");
    upload("/sdcard/config/automation/new.tscfg",409,"automation_configuration_managed");
    upload("/sdcard/config%09/other.txt",400,"storage_path_invalid");
    assert(mutations==before);
    has_text("/sdcard/config/automation/state.json","current");
    has_text("/sdcard/config/ordinary.json","ordinary");
    has_text("/autocfg/state.json","emergency");

    /* Reading/copying out stays available. Ordinary files keep normal operations. */
    assert(ts_storage_copy("/sdcard/config/automation/state.json","/sdcard/export.json")==ESP_OK);
    has_text("/sdcard/export.json","current");
    assert(ts_storage_mkdir_p("/sdcard/work/nested")==ESP_OK);
    FILE *stream=NULL;assert(ts_storage_open_write("/sdcard/work/nested/a",&stream)==ESP_OK&&stream);
    assert(fwrite("a",1,1,stream)==1&&!fclose(stream));
    assert(ts_storage_append("/sdcard/work/nested/a","b",1)==ESP_OK);
    assert(ts_storage_rename("/sdcard/work/nested/a","/sdcard/work/nested/b")==ESP_OK);
    has_text("/sdcard/work/nested/b","ab");
    cJSON *params=cJSON_Parse("{\"path\":\"/sdcard/export.json\"}");ts_api_result_t result={0};
    assert(api_storage_delete(params,&result)==ESP_OK&&result.code==TS_API_OK);
    cJSON_Delete(params);cJSON_Delete(result.data);
    assert(ts_storage_rmdir_r("/sdcard/work")==ESP_OK&&!ts_storage_exists("/sdcard/work"));

    upload("/sdcard/upload.txt",200,"uploaded");
    has_text("/sdcard/upload.txt","uploaded");
    assert(ts_storage_delete("/sdcard/upload.txt")==ESP_OK);

    /* Cleanup through the raw test VFS, since the configuration owner is private. */
    assert(!test_unlink("/sdcard/config/ordinary.json"));assert(!test_unlink("/sdcard/config/automation/state.json"));
    assert(!test_rmdir("/sdcard/config/automation"));assert(!test_rmdir("/sdcard/config"));
    assert(!test_rmdir("/sdcard"));assert(!test_unlink("/autocfg/state.json"));
    assert(!test_rmdir("/autocfg"));assert(!rmdir(root));assert(!pthread_mutex_destroy(&gate));
    puts("PASS storage ownership: actual file/API/upload mutations, whole-parent rejection, FAT alias boundary, read/export and ordinary operations");
}
